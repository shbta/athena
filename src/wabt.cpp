/*
 * Copyright 2016-2018 Alex Beregszaszi et al.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <iostream>

#include "src/binary-reader.h"
#include "src/cast.h"
#include "src/feature.h"
#include "src/interp/binary-reader-interp.h"
#include "src/interp/interp.h"
#include "src/literal.h"
#include "src/option-parser.h"
#include "src/resolve-names.h"
#include "src/stream.h"
#include "src/validator.h"
#include "src/wast-lexer.h"
#include "src/wast-parser.h"

#include "debugging.h"
#include "eei.h"
#include "exceptions.h"
#include "wabt.h"

using namespace std;
using namespace wabt;

namespace athena {

class WabtEthereumInterface : public EthereumInterface {
public:
  explicit WabtEthereumInterface(
    evmc::HostContext& _context,
    bytes_view _code,
    evmc_message const& _msg,
    ExecutionResult & _result,
    bool _meterGas
  ):
    EthereumInterface(_context, _code, _msg, _result, _meterGas)
  {}
  void setEnv(interp::Environment *evP) {
	envPtr = evP;
  }

private:
  // These assume that m_wasmMemory was set prior to execution.
  size_t memorySize() const override {
	auto memPtr = envPtr->GetMemory(0);
	return memPtr->data.size();
  }
  void memorySet(size_t offset, uint8_t value) override {
	auto memPtr = envPtr->GetMemory(0);
	memPtr->data[offset] = static_cast<char>(value);
  }
  uint8_t memoryGet(size_t offset) override {
	auto memPtr = envPtr->GetMemory(0);
	return static_cast<uint8_t>(memPtr->data[offset]);
  }
  uint8_t* memoryPointer(size_t offset, size_t length) override {
	auto memPtr = envPtr->GetMemory(0);
    ensureCondition(memorySize() >= (offset + length), InvalidMemoryAccess, "Memory is shorter than requested segment");
    return reinterpret_cast<uint8_t*>(& memPtr->data[offset]);
  }

  interp::Environment *envPtr;
};

unique_ptr<WasmEngine> WabtEngine::create() {
  return unique_ptr<WasmEngine>{new WabtEngine};
}

ExecutionResult WabtEngine::execute(evmc::HostContext &context, bytes_view code,
                                    bytes_view state_code,
                                    evmc_message const &msg,
                                    bool meterInterfaceGas) {
  instantiationStarted();
#if H_DEBUGGING
  H_DEBUG << "Executing with wabt...\n";
#endif

  // Set up the wabt Environment, which includes the Wasm store
  // and the list of modules used for importing/exporting between modules
  interp::Environment env(Features{});

  // Set up interface to eei host functions
  ExecutionResult result;
  WabtEthereumInterface interface{context, state_code, msg, result,
                                  meterInterfaceGas};

  // Create EEI host module
  // The lifecycle of this pointer is handled by `env`.
  interp::HostModule *hostModule = env.AppendHostModule("ethereum");
  athenaAssert(hostModule, "Failed to create host module.");

  hostModule->AppendFuncExport(
    "useGas",
    {{Type::I64}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.eeiUseGas(static_cast<int64_t>(args[0].value.i64));
      return interp::ResultType::Ok;
    }
  );

  hostModule->AppendFuncExport(
    "getAddress",
    {{Type::I32}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.eeiGetAddress(args[0].value.i32);
      return interp::ResultType::Ok;
    }
  );

  hostModule->AppendFuncExport(
    "getExternalBalance",
    {{Type::I32, Type::I32}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.eeiGetExternalBalance(args[0].value.i32, args[1].value.i32);
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "getBlockHash",
    {{Type::I64, Type::I32}, {Type::I32}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues& results
    ) {
      results[0].set_i32(interface.eeiGetBlockHash(args[0].value.i64, args[1].value.i32));
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "call",
    {{Type::I64, Type::I32, Type::I32, Type::I32, Type::I32}, {Type::I32}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues& results
    ) {
      results[0].set_i32(interface.eeiCall(
        EthereumInterface::EEICallKind::Call,
        static_cast<int64_t>(args[0].value.i64), args[1].value.i32,
        args[2].value.i32, args[3].value.i32, args[4].value.i32
      ));
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "callDataCopy",
    {{Type::I32, Type::I32, Type::I32}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.eeiCallDataCopy(args[0].value.i32, args[1].value.i32, args[2].value.i32);
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "getCallDataSize",
    {{}, {Type::I32}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues&,
      interp::TypedValues& results
    ) {
      results[0].set_i32(interface.eeiGetCallDataSize());
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "callCode",
    {{Type::I64, Type::I32, Type::I32, Type::I32, Type::I32}, {Type::I32}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues& results
    ) {
      results[0].set_i32(interface.eeiCall(
        EthereumInterface::EEICallKind::CallCode,
        static_cast<int64_t>(args[0].value.i64), args[1].value.i32,
        args[2].value.i32, args[3].value.i32, args[4].value.i32
      ));
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "callDelegate",
    {{Type::I64, Type::I32, Type::I32, Type::I32}, {Type::I32}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues& results
    ) {
      results[0].set_i32(interface.eeiCall(
        EthereumInterface::EEICallKind::CallDelegate,
        static_cast<int64_t>(args[0].value.i64), args[1].value.i32, 0,
        args[2].value.i32, args[3].value.i32
      ));
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "callStatic",
    {{Type::I64, Type::I32, Type::I32, Type::I32}, {Type::I32}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues& results
    ) {
      results[0].set_i32(interface.eeiCall(
        EthereumInterface::EEICallKind::CallStatic,
        static_cast<int64_t>(args[0].value.i64), args[1].value.i32, 0,
        args[2].value.i32, args[3].value.i32
      ));
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "storageStore",
    {{Type::I32, Type::I32}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.eeiStorageStore(args[0].value.i32, args[1].value.i32);
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "storageLoad",
    {{Type::I32, Type::I32}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.eeiStorageLoad(args[0].value.i32, args[1].value.i32);
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "getCaller",
    {{Type::I32}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.eeiGetCaller(args[0].value.i32);
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "getCallValue",
    {{Type::I32}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.eeiGetCallValue(args[0].value.i32);
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "codeCopy",
    {{Type::I32, Type::I32, Type::I32}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.eeiCodeCopy(args[0].value.i32, args[1].value.i32, args[2].value.i32);
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "getCodeSize",
    {{}, {Type::I32}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues&,
      interp::TypedValues& results
    ) {
      results[0].set_i32(interface.eeiGetCodeSize());
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "getBlockCoinbase",
    {{Type::I32}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.eeiGetBlockCoinbase(args[0].value.i32);
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "create",
    {{Type::I32, Type::I32, Type::I32, Type::I32}, {Type::I32}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues& results
    ) {
      results[0].set_i32(interface.eeiCreate(
        args[0].value.i32, args[1].value.i32,
        args[2].value.i32, args[3].value.i32
      ));
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "getBlockDifficulty",
    {{Type::I32}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.eeiGetBlockDifficulty(args[0].value.i32);
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "externalCodeCopy",
    {{Type::I32, Type::I32, Type::I32, Type::I32}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.eeiExternalCodeCopy(
        args[0].value.i32, args[1].value.i32,
        args[2].value.i32, args[3].value.i32
      );
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "getExternalCodeSize",
    {{Type::I32}, {Type::I32}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues& results
    ) {
      results[0].set_i32(interface.eeiGetExternalCodeSize(args[0].value.i32));
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "getGasLeft",
    {{}, {Type::I64}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues&,
      interp::TypedValues& results
    ) {
      results[0].set_i64(static_cast<uint64_t>(interface.eeiGetGasLeft()));
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "getBlockGasLimit",
    {{}, {Type::I64}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues&,
      interp::TypedValues& results
    ) {
      results[0].set_i64(static_cast<uint64_t>(interface.eeiGetBlockGasLimit()));
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "getTxGasPrice",
    {{Type::I32}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.eeiGetTxGasPrice(args[0].value.i32);
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "log",
    {{Type::I32, Type::I32, Type::I32, Type::I32, Type::I32, Type::I32, Type::I32}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.eeiLog(
        args[0].value.i32, args[1].value.i32, args[2].value.i32, args[3].value.i32,
        args[4].value.i32, args[5].value.i32, args[6].value.i32
      );
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "getBlockNumber",
    {{}, {Type::I64}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues&,
      interp::TypedValues& results
    ) {
      results[0].set_i64(static_cast<uint64_t>(interface.eeiGetBlockNumber()));
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "getTxOrigin",
    {{Type::I32}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.eeiGetTxOrigin(args[0].value.i32);
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "finish",
    {{Type::I32, Type::I32}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
#if H_DEBUGGING
      interface.debugPrintMem(true, args[0].value.i32, args[1].value.i32);
#endif
      interface.eeiFinish(args[0].value.i32, args[1].value.i32);
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "revert",
    {{Type::I32, Type::I32}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.eeiRevert(args[0].value.i32, args[1].value.i32);
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "getReturnDataSize",
    {{}, {Type::I32}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues&,
      interp::TypedValues& results
    ) {
      results[0].set_i32(interface.eeiGetReturnDataSize());
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "returnDataCopy",
    {{Type::I32, Type::I32, Type::I32}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.eeiReturnDataCopy(args[0].value.i32, args[1].value.i32, args[2].value.i32);
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "selfDestruct",
    {{Type::I32}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.eeiSelfDestruct(args[0].value.i32);
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "getBlockTimestamp",
    {{}, {Type::I64}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues&,
      interp::TypedValues& results
    ) {
      results[0].set_i64(static_cast<uint64_t>(interface.eeiGetBlockTimestamp()));
      return interp::Result(interp::ResultType::Ok);
    }
  );

#if H_DEBUGGING
  // Create debug host module
  // The lifecycle of this pointer is handled by `env`.
  hostModule = env.AppendHostModule("debug");
  athenaAssert(hostModule, "Failed to create host module.");

  hostModule->AppendFuncExport(
    "print",
    {{Type::I32, Type::I32}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.debugPrint(args[0].value.i32, args[1].value.i32);
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "print32",
    {{Type::I32}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.debugPrint32(args[0].value.i32);
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "print64",
    {{Type::I64}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.debugPrint64(args[0].value.i64);
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "printMem",
    {{Type::I32, Type::I32}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.debugPrintMem(false, args[0].value.i32, args[1].value.i32);
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "printMemHex",
    {{Type::I32, Type::I32}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.debugPrintMem(true, args[0].value.i32, args[1].value.i32);
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "printStorage",
    {{Type::I32}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.debugPrintStorage(false, args[0].value.i32);
      return interp::Result(interp::ResultType::Ok);
    }
  );

  hostModule->AppendFuncExport(
    "printStorageHex",
    {{Type::I32}, {}},
    [&interface](
      const interp::HostFunc*,
      const interp::FuncSignature*,
      const interp::TypedValues& args,
      interp::TypedValues&
    ) {
      interface.debugPrintStorage(true, args[0].value.i32);
      return interp::Result(interp::ResultType::Ok);
    }
  );
#endif

  // Parse module
  ReadBinaryOptions options(Features{},
                            nullptr, // debugging stream for loading
                            false,   // ReadDebugNames
                            true,    // StopOnFirstError
                            true     // FailOnCustomSectionError
  );

  Errors errors;
  interp::DefinedModule *module = nullptr;
  Result loadResult = ReadBinaryInterp(&env, code.data(), code.size(), options,
                                       &errors, &module);

#if H_DEBUGGING
  for (auto it = errors.begin(); it != errors.end(); ++it) {
    H_DEBUG << "wabt (execute): " << it->message << "\n";
  }
#endif

  ensureCondition(Succeeded(loadResult) && module, ContractValidationFailure,
                  "Module failed to load.");
  ensureCondition(env.GetMemoryCount() == 1, ContractValidationFailure,
                  "Multiple memory sections exported.");
  ensureCondition(module->GetExport("memory"), ContractValidationFailure,
                  "\"memory\" not found");
  ensureCondition(module->start_func_index == kInvalidIndex,
                  ContractValidationFailure,
                  "Contract contains start function.");

  // Prepare to execute
  interp::Export *mainFunction = module->GetExport("main");
  ensureCondition(mainFunction, ContractValidationFailure,
                  "\"main\" not found");
  ensureCondition(mainFunction->kind == ExternalKind::Func,
                  ContractValidationFailure, "\"main\" is not a function");

  interp::Executor executor(&env,
                            nullptr,                  // null for no tracing
                            interp::Thread::Options{} // empty for no threads
  );

  // better set env other than setMemory
  interface.setEnv(&env);
  executionStarted();

  // Execute main
  try {
    interp::ExecResult wabtResult = executor.Initialize(module);
    ensureCondition(wabtResult.result.ok(), VMTrap, "VM initialize failed.");
    wabtResult = executor.RunExport(
        mainFunction,
        interp::TypedValues{}); // second arg is empty since no args
    // Wrap any non-EEI exception under VMTrap.
    ensureCondition(wabtResult.result.ok(), VMTrap, "The VM invocation had a trap.");
  } catch (EndExecution const&) {
    // This exception is ignored here because we consider it to be a success.
    // It is only a clutch for POSIX style exit()
  }

  executionFinished();
  return result;
}

} // namespace athena
