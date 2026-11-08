#include "Cafe/HW/Espresso/Interpreter/PPCInterpreterInternal.h"
#include "PPCFunctionBoundaryTracker.h"
#include "PPCRecompiler.h"
#include "PPCRecompilerIml.h"
#include "Cafe/OS/RPL/rpl.h"
#include "util/containers/RangeStore.h"
#include "Cafe/OS/libs/coreinit/coreinit_CodeGen.h"
#include "config/ActiveSettings.h"
#include "config/LaunchSettings.h"
#include "Common/ExceptionHandler/ExceptionHandler.h"
#include "Common/cpu_features.h"
#include "util/helpers/fspinlock.h"
#include "util/helpers/helpers.h"
#include "util/MemMapper/MemMapper.h"

#include "IML/IML.h"
#include "IML/IMLRegisterAllocator.h"
#include "BackendX64/BackendX64.h"
#ifdef __aarch64__
#include "BackendAArch64/BackendAArch64.h"
#endif
#include "util/highresolutiontimer/HighResolutionTimer.h"
#include "util/CemuProfiling.h"
#include "util/CemuThreadWatchdog.h"
#include "util/CemuDiagV2.h"

#define PPCREC_FORCE_SYNCHRONOUS_COMPILATION	0 // if 1, then function recompilation will block and execute on the thread that called PPCRecompiler_visitAddressNoBlock
#define PPCREC_LOG_RECOMPILATION_RESULTS		0

struct PPCInvalidationRange
{
	MPTR startAddress;
	uint32 size;

	PPCInvalidationRange(MPTR _startAddress, uint32 _size) : startAddress(_startAddress), size(_size) {};
};

struct
{
	FSpinlock recompilerSpinlock;
	std::queue<MPTR> targetQueue;
	std::queue<MPTR> targetQueueHigh;  // priority queue for 0x0675 zone (main thread)
	std::vector<PPCInvalidationRange> invalidationRanges;
}PPCRecompilerState;

RangeStore<PPCRecFunction_t*, uint32, 7703, 0x2000> rangeStore_ppcRanges;

void ATTR_MS_ABI (*PPCRecompiler_enterRecompilerCode)(uint64 codeMem, uint64 ppcInterpreterInstance);
void ATTR_MS_ABI (*PPCRecompiler_leaveRecompilerCode_visited)();
void ATTR_MS_ABI (*PPCRecompiler_leaveRecompilerCode_unvisited)();

PPCRecompilerInstanceData_t* ppcRecompilerInstanceData;

#if PPCREC_FORCE_SYNCHRONOUS_COMPILATION
static std::mutex s_singleRecompilationMutex;
#endif

bool ppcRecompilerEnabled = false;

namespace CemuDiagV2
{
    std::atomic<uint64_t> g_jitCount{0};
    std::atomic<uint64_t> g_jitTotalNs{0};
    std::atomic<uint64_t> g_jitMaxNs{0};
    std::mutex g_jitTop5Mtx;
    std::map<uint32_t, uint64_t> g_jitTop5Map;

    std::atomic<uint64_t> g_vkDrawCount{0};
    std::atomic<uint64_t> g_vkSubmitCount{0};
    std::atomic<uint64_t> g_vkSubmitNs{0};
    std::atomic<uint64_t> g_vkPresentCount{0};
    std::atomic<uint64_t> g_vkPresentNs{0};
}

void PPCRecompiler_recompileAtAddress(uint32 address);

// this function does never block and can fail if the recompiler lock cannot be acquired immediately
void PPCRecompiler_visitAddressNoBlock(uint32 enterAddress)
{
#if PPCREC_FORCE_SYNCHRONOUS_COMPILATION
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] != PPCRecompiler_leaveRecompilerCode_unvisited)
		return;
	PPCRecompilerState.recompilerSpinlock.lock();
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] != PPCRecompiler_leaveRecompilerCode_unvisited)
	{
		PPCRecompilerState.recompilerSpinlock.unlock();
		return;
	}
	ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] = PPCRecompiler_leaveRecompilerCode_visited;
	PPCRecompilerState.recompilerSpinlock.unlock();
	s_singleRecompilationMutex.lock();
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] == PPCRecompiler_leaveRecompilerCode_visited)
	{
		PPCRecompiler_recompileAtAddress(enterAddress);
	}
	s_singleRecompilationMutex.unlock();
	return;
#endif
	// quick read-only check without lock
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] != PPCRecompiler_leaveRecompilerCode_unvisited)
		return;
	// try to acquire lock
	if (!PPCRecompilerState.recompilerSpinlock.try_lock())
	{
		if ((enterAddress & 0xFFFF0000u) == 0x06750000u)
			cemuLog_log(LogType::Force, "[VISIT-0675] addr=0x{:08X} LOCK_FAILED", enterAddress);
		return;
	}
	auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
	if (funcPtr != PPCRecompiler_leaveRecompilerCode_unvisited)
	{
		// was visited since previous check
		PPCRecompilerState.recompilerSpinlock.unlock();
		return;
	}
	// add to recompilation queue and flag as visited
	if (enterAddress >= 0x06700000u && enterAddress < 0x06800000u)
	{
		PPCRecompilerState.targetQueueHigh.emplace(enterAddress);
		cemuLog_log(LogType::Force, "[VISIT-067X] addr=0x{:08X} QUEUED_HIGH size_high={}", enterAddress, PPCRecompilerState.targetQueueHigh.size());
	}
	else
	{
		PPCRecompilerState.targetQueue.emplace(enterAddress);
	}
	ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4] = PPCRecompiler_leaveRecompilerCode_visited;

	PPCRecompilerState.recompilerSpinlock.unlock();
}

void PPCRecompiler_recompileIfUnvisited(uint32 enterAddress)
{
	if (ppcRecompilerEnabled == false)
		return;
	PPCRecompiler_visitAddressNoBlock(enterAddress);
}

void PPCRecompiler_enter(PPCInterpreter_t* hCPU, PPCREC_JUMP_ENTRY funcPtr)
{
#if BOOST_OS_WINDOWS
	uint32 prevState = _controlfp(0, 0);
	_controlfp(_RC_NEAR, _MCW_RC);
	PPCRecompiler_enterRecompilerCode((uint64)funcPtr, (uint64)hCPU);
	_controlfp(prevState, _MCW_RC);
	// debug recompiler exit - useful to find frequently executed functions which couldn't be recompiled
	#ifdef CEMU_DEBUG_ASSERT
	if (hCPU->remainingCycles > 0 && GetAsyncKeyState(VK_F4))
	{
		auto t = std::chrono::high_resolution_clock::now();
		auto dur = std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
		cemuLog_log(LogType::Force, "Recompiler exit: 0x{:08x} LR: 0x{:08x} Timestamp {}.{:04}", hCPU->instructionPointer, hCPU->spr.LR, dur / 1000LL, (dur % 1000LL));
	}
	#endif
#else
	PPCRecompiler_enterRecompilerCode((uint64)funcPtr, (uint64)hCPU);
#endif
	// after leaving recompiler prematurely attempt to recompile the code at the new location
	if (hCPU->remainingCycles > 0)
	{
		PPCRecompiler_visitAddressNoBlock(hCPU->instructionPointer);
	}
}

void PPCRecompiler_attemptEnterWithoutRecompile(PPCInterpreter_t* hCPU, uint32 enterAddress)
{
	cemu_assert_debug(hCPU->instructionPointer == enterAddress);
	if (ppcRecompilerEnabled == false)
		return;
	auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
	if (funcPtr != PPCRecompiler_leaveRecompilerCode_unvisited && funcPtr != PPCRecompiler_leaveRecompilerCode_visited)
	{
		cemu_assert_debug(ppcRecompilerInstanceData != nullptr);
		PPCRecompiler_enter(hCPU, funcPtr);
	}
}

void PPCRecompiler_attemptEnter(PPCInterpreter_t* hCPU, uint32 enterAddress)
{
	cemu_assert_debug(hCPU->instructionPointer == enterAddress);
	if (ppcRecompilerEnabled == false)
		return;
	if ((enterAddress & 0xFFFF0000u) == 0x06750000u)
		cemuLog_log(LogType::Force, "[ATTEMPT-0675] addr=0x{:08X} rc={} en={}", enterAddress, hCPU->remainingCycles, ppcRecompilerEnabled);
	if (hCPU->remainingCycles <= 0)
		return;
	auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
	if (funcPtr == PPCRecompiler_leaveRecompilerCode_unvisited)
	{
		PPCRecompiler_visitAddressNoBlock(enterAddress);
	}
	else if (funcPtr != PPCRecompiler_leaveRecompilerCode_visited)
	{
		// enter
		cemu_assert_debug(ppcRecompilerInstanceData != nullptr);
		PPCRecompiler_enter(hCPU, funcPtr);
	}
}
bool PPCRecompiler_ApplyIMLPasses(ppcImlGenContext_t& ppcImlGenContext);

PPCRecFunction_t* PPCRecompiler_recompileFunction(PPCFunctionBoundaryTracker::PPCRange_t range, std::set<uint32>& entryAddresses, std::vector<std::pair<MPTR, uint32>>& entryPointsOut, PPCFunctionBoundaryTracker& boundaryTracker)
{
#if CEMU_THREAD_WATCHDOG
    CemuWatchdog::Touch(CemuWatchdog::Recompiler, CemuWatchdog::Compile);
#endif
#if CEMU_PROFILING
    CemuProfiling::ScopedTimer profilingTimer(CemuProfiling::Recompiler());
#endif
    CemuDiagV2::ScopedJitTimer diagJitTimer(range.startAddress);
	if (range.startAddress >= PPC_REC_CODE_AREA_END)
	{
		cemuLog_log(LogType::Force, "Attempting to recompile function outside of allowed code area");
		return nullptr;
	}
	uint32 codeGenRangeStart;
	uint32 codeGenRangeSize = 0;
	coreinit::OSGetCodegenVirtAddrRangeInternal(codeGenRangeStart, codeGenRangeSize);
	if (codeGenRangeSize != 0)
	{
		if (range.startAddress >= codeGenRangeStart && range.startAddress < (codeGenRangeStart + codeGenRangeSize))
		{
			if (coreinit::codeGenShouldAvoid())
			{
				return nullptr;
			}
		}
	}

	PPCRecFunction_t* ppcRecFunc = new PPCRecFunction_t();
	ppcRecFunc->ppcAddress = range.startAddress;
	ppcRecFunc->ppcSize = range.length;

#if PPCREC_LOG_RECOMPILATION_RESULTS
	BenchmarkTimer bt;
	bt.Start();
#endif

	// generate intermediate code
	ppcImlGenContext_t ppcImlGenContext = { 0 };
	ppcImlGenContext.debug_entryPPCAddress = range.startAddress;
	auto phaseStart = std::chrono::steady_clock::now();
    bool compiledSuccessfully = PPCRecompiler_generateIntermediateCode(ppcImlGenContext, ppcRecFunc, entryAddresses, boundaryTracker);
    uint64_t imlMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - phaseStart).count();
	cemuLog_log(LogType::Force, "[PHASE] 0x{:08X} generateIML done ok={}", range.startAddress, compiledSuccessfully);
	if (compiledSuccessfully == false)
	{
		delete ppcRecFunc;
		return nullptr;
	}

	uint32 ppcRecLowerAddr = LaunchSettings::GetPPCRecLowerAddr();
	uint32 ppcRecUpperAddr = LaunchSettings::GetPPCRecUpperAddr();

	if (ppcRecLowerAddr != 0 && ppcRecUpperAddr != 0)
	{
		if (ppcRecFunc->ppcAddress < ppcRecLowerAddr || ppcRecFunc->ppcAddress > ppcRecUpperAddr)
		{
			delete ppcRecFunc;
			return nullptr;
		}
	}

	// apply passes
	phaseStart = std::chrono::steady_clock::now();
    bool passesOk = PPCRecompiler_ApplyIMLPasses(ppcImlGenContext);
    uint64_t passMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - phaseStart).count();
    if (!passesOk)
	{
		delete ppcRecFunc;
		return nullptr;
	}
	cemuLog_log(LogType::Force, "[PHASE] 0x{:08X} applyIMLPasses done", range.startAddress);

#if defined(ARCH_X86_64)
	// emit x64 code
	bool x64GenerationSuccess = PPCRecompiler_generateX64Code(ppcRecFunc, &ppcImlGenContext);
	if (x64GenerationSuccess == false)
	{
		return nullptr;
	}
#elif defined(__aarch64__)
	phaseStart = std::chrono::steady_clock::now();
    bool aarch64GenerationSuccess = PPCRecompiler_generateAArch64Code(ppcRecFunc, &ppcImlGenContext);
    uint64_t a64Ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - phaseStart).count();
    
    if (imlMs + passMs + a64Ms > 100)
        cemuLog_log(LogType::Force, "[PHASE SLOW] pc=0x{:08X} IML={}ms Opt={}ms A64={}ms", range.startAddress, imlMs, passMs, a64Ms);

	cemuLog_log(LogType::Force, "[PHASE] 0x{:08X} generateAArch64Code done ok={}", range.startAddress, aarch64GenerationSuccess);
	if (aarch64GenerationSuccess == false)
	{
		return nullptr;
	}
#endif
	if (ActiveSettings::DumpRecompilerFunctionsEnabled())
	{
		FileStream* fs = FileStream::createFile2(ActiveSettings::GetUserDataPath(fmt::format("dump/recompiler/ppc_{:08x}.bin", ppcRecFunc->ppcAddress)));
		if (fs)
		{
			fs->writeData(ppcRecFunc->x86Code, ppcRecFunc->x86Size);
			delete fs;
		}
	}

	// collect list of PPC-->x64 entry points
	entryPointsOut.clear();
	for(IMLSegment* imlSegment : ppcImlGenContext.segmentList2)
	{
		if (imlSegment->isEnterable == false)
			continue;

		uint32 ppcEnterOffset = imlSegment->enterPPCAddress;
		uint32 x64Offset = imlSegment->x64Offset;

		entryPointsOut.emplace_back(ppcEnterOffset, x64Offset);
	}

#if PPCREC_LOG_RECOMPILATION_RESULTS
	bt.Stop();
	uint32 codeHash = 0;
	for (uint32 i = 0; i < ppcRecFunc->x86Size; i++)
	{
		codeHash = _rotr(codeHash, 3);
		codeHash += ((uint8*)ppcRecFunc->x86Code)[i];
	}
	cemuLog_log(LogType::Force, "[Recompiler] PPC 0x{:08x} -> x64: 0x{:x} Took {:.4}ms | Size {:04x} CodeHash {:08x}", (uint32)ppcRecFunc->ppcAddress, (uint64)(uintptr_t)ppcRecFunc->x86Code, bt.GetElapsedMilliseconds(), ppcRecFunc->x86Size, codeHash);
#endif

	return ppcRecFunc;
}

void PPCRecompiler_NativeRegisterAllocatorPass(ppcImlGenContext_t& ppcImlGenContext)
{
	IMLRegisterAllocatorParameters raParam;

	for (auto& it : ppcImlGenContext.mappedRegs)
		raParam.regIdToName.try_emplace(it.second.GetRegID(), it.first);

#if defined(ARCH_X86_64)
	auto& gprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::I64);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RAX);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RDX);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RBX);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RBP);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RSI);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RDI);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R8);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R9);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R10);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R11);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_R12);
	gprPhysPool.SetAvailable(IMLArchX86::PHYSREG_GPR_BASE + X86_REG_RCX);

	// add XMM registers, except XMM15 which is the temporary register
	auto& fprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::F64);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 0);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 1);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 2);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 3);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 4);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 5);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 6);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 7);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 8);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 9);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 10);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 11);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 12);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 13);
	fprPhysPool.SetAvailable(IMLArchX86::PHYSREG_FPR_BASE + 14);
#elif defined(__aarch64__)
	auto& gprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::I64);
	for (auto i = IMLArchAArch64::PHYSREG_GPR_BASE; i < IMLArchAArch64::PHYSREG_GPR_BASE + IMLArchAArch64::PHYSREG_GPR_COUNT; i++)
	{
		if (i == IMLArchAArch64::PHYSREG_GPR_BASE + 18)
			continue; // Skip reserved platform register
		gprPhysPool.SetAvailable(i);
	}

	auto& fprPhysPool = raParam.GetPhysRegPool(IMLRegFormat::F64);
	for (auto i = IMLArchAArch64::PHYSREG_FPR_BASE; i < IMLArchAArch64::PHYSREG_FPR_BASE + IMLArchAArch64::PHYSREG_FPR_COUNT; i++)
		fprPhysPool.SetAvailable(i);
#endif

	IMLRegisterAllocator_AllocateRegisters(&ppcImlGenContext, raParam);
}

bool PPCRecompiler_ApplyIMLPasses(ppcImlGenContext_t& ppcImlGenContext)
{
    auto t0 = std::chrono::steady_clock::now();
    
	PPCRecompilerIML_isolateEnterableSegments(&ppcImlGenContext);
    auto t1 = std::chrono::steady_clock::now();

	IMLOptimizer_OptimizeDirectFloatCopies(&ppcImlGenContext);
	IMLOptimizer_OptimizeDirectIntegerCopies(&ppcImlGenContext);
    auto t2 = std::chrono::steady_clock::now();

	IMLOptimizer_StandardOptimizationPass(ppcImlGenContext);
    auto t3 = std::chrono::steady_clock::now();

	PPCRecompiler_NativeRegisterAllocatorPass(ppcImlGenContext);
    auto t4 = std::chrono::steady_clock::now();

    auto ms = [](auto a, auto b) { return std::chrono::duration_cast<std::chrono::milliseconds>(b - a).count(); };
    uint64_t total = ms(t0, t4);
    
    if (total > 100) {
        cemuLog_log(LogType::Force, "[OPT SLOW] isolate={}ms float/int={}ms std={}ms regalloc={}ms total={}ms",
            ms(t0,t1), ms(t1,t2), ms(t2,t3), ms(t3,t4), total);
    }

	return true;
}

bool PPCRecompiler_makeRecompiledFunctionActive(uint32 initialEntryPoint, PPCFunctionBoundaryTracker::PPCRange_t& range, PPCRecFunction_t* ppcRecFunc, std::vector<std::pair<MPTR, uint32>>& entryPoints)
{
	// update jump table
	PPCRecompilerState.recompilerSpinlock.lock();

	// check if the initial entrypoint is still flagged for recompilation
	// its possible that the range has been invalidated during the time it took to translate the function
	if (ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[initialEntryPoint / 4] != PPCRecompiler_leaveRecompilerCode_visited)
	{
		PPCRecompilerState.recompilerSpinlock.unlock();
		return false;
	}

	// check if the current range got invalidated during the time it took to recompile it
	bool isInvalidated = false;
	for (auto& invRange : PPCRecompilerState.invalidationRanges)
	{
		MPTR rStartAddr = invRange.startAddress;
		MPTR rEndAddr = rStartAddr + invRange.size;
		for (auto& recFuncRange : ppcRecFunc->list_ranges)
		{
			if (recFuncRange.ppcAddress < (rEndAddr) && (recFuncRange.ppcAddress + recFuncRange.ppcSize) >= rStartAddr)
			{
				isInvalidated = true;
				break;
			}
		}
	}
	PPCRecompilerState.invalidationRanges.clear();
	if (isInvalidated)
	{
		PPCRecompilerState.recompilerSpinlock.unlock();
		return false;
	}


	// update jump table
	for (auto& itr : entryPoints)
	{
		ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[itr.first / 4] = (PPCREC_JUMP_ENTRY)((uint8*)ppcRecFunc->x86Code + itr.second);
	}


	// due to inlining, some entrypoints can get optimized away
	// therefore we reset all addresses that are still marked as visited (but not recompiled)
	// we dont remove the points from the queue but any address thats not marked as visited won't get recompiled
	// if they are reachable, the interpreter will queue them again
	if (range.startAddress <= 0x0675FFFFu && (range.startAddress + range.length) >= 0x06750000u)
		cemuLog_log(LogType::Force, "[MAKE-ACTIVE-CONTAINS-0675] start=0x{:08X} len=0x{:X} end=0x{:08X}", range.startAddress, range.length, range.startAddress + range.length);
	uint32 resetCount0675 = 0;
	for (uint32 v = range.startAddress; v <= (range.startAddress + range.length); v += 4)
	{
		auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[v / 4];
		if (funcPtr == PPCRecompiler_leaveRecompilerCode_visited)
		{
			ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[v / 4] = PPCRecompiler_leaveRecompilerCode_unvisited;
			if ((v & 0xFFFF0000u) == 0x06750000u)
				resetCount0675++;
		}
	}
	if (resetCount0675 > 0)
		cemuLog_log(LogType::Force, "[RESET-0675] reset {} entries in range 0x{:08X}-0x{:08X}", resetCount0675, range.startAddress, range.startAddress + range.length);

	// register ranges
	for (auto& r : ppcRecFunc->list_ranges)
	{
		r.storedRange = rangeStore_ppcRanges.storeRange(ppcRecFunc, r.ppcAddress, r.ppcAddress + r.ppcSize);
	}
	PPCRecompilerState.recompilerSpinlock.unlock();


	return true;
}

void PPCRecompiler_recompileAtAddress(uint32 address)
{
	cemu_assert_debug(ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[address / 4] == PPCRecompiler_leaveRecompilerCode_visited);
	cemuLog_log(LogType::Force, "[RECOMPILE] addr=0x{:08X}", address);

	// get size
	PPCFunctionBoundaryTracker funcBoundaries;
	funcBoundaries.trackStartPoint(address);
	// get range that encompasses address
	PPCFunctionBoundaryTracker::PPCRange_t range;
	if (funcBoundaries.getRangeForAddress(address, range) == false)
	{
		cemu_assert_debug(false);
	}

	// todo - use info from previously compiled ranges to determine full size of this function (and merge all the entryAddresses)

	// collect all currently known entry points for this range
	PPCRecompilerState.recompilerSpinlock.lock();

	std::set<uint32> entryAddresses;

	entryAddresses.emplace(address);

	PPCRecompilerState.recompilerSpinlock.unlock();

	std::vector<std::pair<MPTR, uint32>> functionEntryPoints;
	auto func = PPCRecompiler_recompileFunction(range, entryAddresses, functionEntryPoints, funcBoundaries);

	if (!func)
	{
		return; // recompilation failed
	}
	bool r = PPCRecompiler_makeRecompiledFunctionActive(address, range, func, functionEntryPoints);
}

std::thread s_threadRecompiler;
std::atomic_bool s_recompilerThreadStopSignal{false};


#if CEMU_PROFILING
static void PPCRecompiler_profileReportIfDue()
{
    using namespace CemuProfiling;
    static auto lastReport = Clock::now();
    const auto now = Clock::now();
    const double seconds = std::chrono::duration<double>(now - lastReport).count();
    if (seconds < 5.0)
        return;
    lastReport = now;

    const auto itp = Interpreter().TakeSnapshot();
    const auto hle = HLE().TakeSnapshot();
    const auto rec = Recompiler().TakeSnapshot();
    const auto fiber = FiberTimeslice().TakeSnapshot();
    const auto draws = Drawcalls().TakeSnapshot();
    const auto pipelines = PipelineChanges().TakeSnapshot();
    const auto sleepWait = ThreadSleepWait().TakeSnapshot();
    const auto joinWait = ThreadJoinWait().TakeSnapshot();
    const auto queueWait = ThreadQueueWait().TakeSnapshot();
    const auto submit = VulkanSubmit().TakeSnapshot();
    const auto present = VulkanPresent().TakeSnapshot();
    const auto fence = VulkanFenceWait().TakeSnapshot();
    const auto audio = AudioUpdate().TakeSnapshot();
    const auto audioLock = AudioMutexWait().TakeSnapshot();

    const auto rate = [seconds](uint64_t value) { return static_cast<double>(value) / seconds; };
    const auto avgMs = [](const Counter::Snapshot& s) {
        return s.calls ? (static_cast<double>(s.nanoseconds) / 1000000.0) / s.calls : 0.0;
    };

    cemuLog_log(LogType::Force,
        "[PROFILE 5s] PPC interp={} instr/s ({:.2f} ms total); HLE={} calls/s avg={:.3f} ms; "
        "JIT={} funcs/s avg={:.3f} ms; fiber={} instr/s avgSlice={:.3f} ms; "
        "GX2 drawcalls={} /s; pipelineBinds={} /s; "
        "threadWait sleep={} /s avg={:.3f} ms join={} /s avg={:.3f} ms queue={} /s avg={:.3f} ms; "
        "vkSubmit={} /s avg={:.3f} ms; "
        "vkPresent={} /s avg={:.3f} ms; fenceWait={} /s avg={:.3f} ms; "
        "AXUpdate={} /s avg={:.3f} ms; AXMutexWait={} /s avg={:.3f} ms",
        rate(itp.items), static_cast<double>(itp.nanoseconds) / 1000000.0,
        rate(hle.calls), avgMs(hle),
        rate(rec.calls), avgMs(rec),
        rate(fiber.items), avgMs(fiber),
        rate(draws.items), rate(pipelines.items),
        rate(sleepWait.calls), avgMs(sleepWait),
        rate(joinWait.calls), avgMs(joinWait),
        rate(queueWait.calls), avgMs(queueWait),
        rate(submit.calls), avgMs(submit),
        rate(present.calls), avgMs(present),
        rate(fence.calls), avgMs(fence),
        rate(audio.calls), avgMs(audio),
        rate(audioLock.calls), avgMs(audioLock));

    const uint64_t diagJitCount = CemuDiagV2::g_jitCount.exchange(0, std::memory_order_relaxed);
    const uint64_t diagJitTotalNs = CemuDiagV2::g_jitTotalNs.exchange(0, std::memory_order_relaxed);
    const uint64_t diagJitMaxNs = CemuDiagV2::g_jitMaxNs.exchange(0, std::memory_order_relaxed);
    const double diagJitAvgMs = diagJitCount ? (static_cast<double>(diagJitTotalNs) / 1000000.0) / diagJitCount : 0.0;
    const double diagJitMaxMs = static_cast<double>(diagJitMaxNs) / 1000000.0;
    cemuLog_log(LogType::Force, "[JIT] n={} avg={:.3f}ms max={:.3f}ms",
        diagJitCount, diagJitAvgMs, diagJitMaxMs);

    std::vector<std::pair<uint32_t, uint64_t>> diagTop5;
    {
        std::lock_guard<std::mutex> lock(CemuDiagV2::g_jitTop5Mtx);
        diagTop5.assign(CemuDiagV2::g_jitTop5Map.begin(), CemuDiagV2::g_jitTop5Map.end());
        CemuDiagV2::g_jitTop5Map.clear();
    }
    std::sort(diagTop5.begin(), diagTop5.end(),
        [](const auto& a, const auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < diagTop5.size() && i < 5; ++i)
        cemuLog_log(LogType::Force, "[JIT TOP] rank={} pc=0x{:08X} count={}",
            i, diagTop5[i].first, diagTop5[i].second);

    const uint64_t diagVkDraw     = CemuDiagV2::g_vkDrawCount.exchange(0, std::memory_order_relaxed);
    const uint64_t diagVkSubmit   = CemuDiagV2::g_vkSubmitCount.exchange(0, std::memory_order_relaxed);
    const uint64_t diagVkSubmitNs = CemuDiagV2::g_vkSubmitNs.exchange(0, std::memory_order_relaxed);
    const uint64_t diagVkPresent  = CemuDiagV2::g_vkPresentCount.exchange(0, std::memory_order_relaxed);
    const uint64_t diagVkPresentNs= CemuDiagV2::g_vkPresentNs.exchange(0, std::memory_order_relaxed);
    cemuLog_log(LogType::Force,
        "[VK FLOW] draw={} submit={} submit_ns={} present={} present_ns={}",
        diagVkDraw, diagVkSubmit, diagVkSubmitNs, diagVkPresent, diagVkPresentNs);
}
#endif

void PPCRecompiler_thread()
{
	SetThreadName("PPCRecompiler");
#ifdef __linux__
	nice(10); // Lower priority for JIT compiler to avoid starving emulation
#endif
    cemuLog_log(LogType::Force, "[WATCHDOG CALLSITE] PPCRecompiler_thread reached");
    const bool watchdogStarted = CemuWatchdog::Start();
    cemuLog_log(LogType::Force, "[WATCHDOG CALLSITE] Start returned {}", watchdogStarted);
#if PPCREC_FORCE_SYNCHRONOUS_COMPILATION
	return;
#endif

	while (true)
	{
        if(s_recompilerThreadStopSignal)
            return;
		#if CEMU_THREAD_WATCHDOG
        CemuWatchdog::Touch(CemuWatchdog::Recompiler, CemuWatchdog::Sleep);
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
#if CEMU_PROFILING
        PPCRecompiler_profileReportIfDue();
#endif
		// asynchronous recompilation:
		// 1) take address from queue
		// 2) check if address is still marked as visited
		// 3) if yes -> calculate size, gather all entry points, recompile and update jump table
		uint32 popInThisBatch = 0;
		constexpr uint32 MAX_POP_PER_BATCH = 64;
		while (true)
		{
			if (popInThisBatch >= MAX_POP_PER_BATCH)
				break;
			popInThisBatch++;
			PPCRecompilerState.recompilerSpinlock.lock();
			MPTR enterAddress = 0;
			if (!PPCRecompilerState.targetQueueHigh.empty())
			{
				enterAddress = PPCRecompilerState.targetQueueHigh.front();
				PPCRecompilerState.targetQueueHigh.pop();
			}
			else if (!PPCRecompilerState.targetQueue.empty())
			{
				enterAddress = PPCRecompilerState.targetQueue.front();
				PPCRecompilerState.targetQueue.pop();
			}
			else
			{
				PPCRecompilerState.recompilerSpinlock.unlock();
				break;
			}
			if (enterAddress >= 0x06700000u && enterAddress < 0x06800000u)
				cemuLog_log(LogType::Force, "[POP-067X] addr=0x{:08X} size_high={} size_low={}", enterAddress, PPCRecompilerState.targetQueueHigh.size(), PPCRecompilerState.targetQueue.size());
			static std::atomic<uint32> popCount{0};
			if ((popCount.fetch_add(1, std::memory_order_relaxed) % 1000) == 0)
				cemuLog_log(LogType::Force, "[QUEUE] size_high={} size_low={} total_pop={}", PPCRecompilerState.targetQueueHigh.size(), PPCRecompilerState.targetQueue.size(), popCount.load());

			auto funcPtr = ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[enterAddress / 4];
			if (funcPtr != PPCRecompiler_leaveRecompilerCode_visited)
			{
				// only recompile functions if marked as visited
				PPCRecompilerState.recompilerSpinlock.unlock();
				continue;
			}
			PPCRecompilerState.recompilerSpinlock.unlock();

			PPCRecompiler_recompileAtAddress(enterAddress);
			if(s_recompilerThreadStopSignal)
				return;
		}
	}
}

#define PPC_REC_ALLOC_BLOCK_SIZE	(4*1024*1024) // 4MB

constexpr uint32 PPCRecompiler_GetNumAddressSpaceBlocks()
{
    return (MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE + PPC_REC_ALLOC_BLOCK_SIZE - 1) / PPC_REC_ALLOC_BLOCK_SIZE;
}

std::bitset<PPCRecompiler_GetNumAddressSpaceBlocks()> ppcRecompiler_reservedBlockMask;

void PPCRecompiler_reserveLookupTableBlock(uint32 offset)
{
	uint32 blockIndex = offset / PPC_REC_ALLOC_BLOCK_SIZE;
	offset = blockIndex * PPC_REC_ALLOC_BLOCK_SIZE;

	if (ppcRecompiler_reservedBlockMask[blockIndex])
		return;
	ppcRecompiler_reservedBlockMask[blockIndex] = true;

	void* p1 = MemMapper::AllocateMemory(&(ppcRecompilerInstanceData->ppcRecompilerFuncTable[offset/4]), (PPC_REC_ALLOC_BLOCK_SIZE/4)*sizeof(void*), MemMapper::PAGE_PERMISSION::P_RW, true);
	void* p3 = MemMapper::AllocateMemory(&(ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[offset/4]), (PPC_REC_ALLOC_BLOCK_SIZE/4)*sizeof(void*), MemMapper::PAGE_PERMISSION::P_RW, true);
	if( !p1 || !p3 )
	{
		cemuLog_log(LogType::Force, "Failed to allocate memory for recompiler (0x{:08x})", offset);
		cemu_assert(false);
		return;
	}
	for(uint32 i=0; i<PPC_REC_ALLOC_BLOCK_SIZE/4; i++)
	{
		ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[offset/4+i] = PPCRecompiler_leaveRecompilerCode_unvisited;
	}
}

void PPCRecompiler_allocateRange(uint32 startAddress, uint32 size)
{
	if (ppcRecompilerInstanceData == nullptr)
		return;
	uint32 endAddress = (startAddress + size + PPC_REC_ALLOC_BLOCK_SIZE - 1) & ~(PPC_REC_ALLOC_BLOCK_SIZE-1);
	startAddress = (startAddress) & ~(PPC_REC_ALLOC_BLOCK_SIZE-1);
	startAddress = std::min(startAddress, (uint32)MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE);
	endAddress = std::min(endAddress, (uint32)MEMORY_CODEAREA_ADDR + MEMORY_CODEAREA_SIZE);
	for (uint32 i = startAddress; i < endAddress; i += PPC_REC_ALLOC_BLOCK_SIZE)
	{
		PPCRecompiler_reserveLookupTableBlock(i);
	}
}

struct ppcRecompilerFuncRange_t
{
	MPTR	ppcStart;
	uint32  ppcSize;
	void*   x86Start;
	size_t  x86Size;
};

bool PPCRecompiler_findFuncRanges(uint32 addr, ppcRecompilerFuncRange_t* rangesOut, size_t* countInOut)
{
	PPCRecompilerState.recompilerSpinlock.lock();
	size_t countIn = *countInOut;
	size_t countOut = 0;

	rangeStore_ppcRanges.findRanges(addr, addr + 4, [rangesOut, countIn, &countOut](uint32 start, uint32 end, PPCRecFunction_t* func)
	{
		if (countOut < countIn)
		{
			rangesOut[countOut].ppcStart = start;
			rangesOut[countOut].ppcSize = (end-start);
			rangesOut[countOut].x86Start = func->x86Code;
			rangesOut[countOut].x86Size = func->x86Size;
		}
		countOut++;
	}
	);
	PPCRecompilerState.recompilerSpinlock.unlock();
	*countInOut = countOut;
	if (countOut > countIn)
		return false;
	return true;
}

extern "C" DLLEXPORT uintptr_t * PPCRecompiler_getJumpTableBase()
{
	if (ppcRecompilerInstanceData == nullptr)
		return nullptr;
	return (uintptr_t*)ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable;
}

void PPCRecompiler_invalidateTableRange(uint32 offset, uint32 size)
{
	if (ppcRecompilerInstanceData == nullptr)
		return;
	for (uint32 i = 0; i < size / 4; i++)
	{
		ppcRecompilerInstanceData->ppcRecompilerFuncTable[offset / 4 + i] = nullptr;
		ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[offset / 4 + i] = PPCRecompiler_leaveRecompilerCode_unvisited;
	}
}

void PPCRecompiler_deleteFunction(PPCRecFunction_t* func)
{
	// assumes PPCRecompilerState.recompilerSpinlock is already held
	cemu_assert_debug(PPCRecompilerState.recompilerSpinlock.is_locked());
	for (auto& r : func->list_ranges)
	{
		PPCRecompiler_invalidateTableRange(r.ppcAddress, r.ppcSize);
		if(r.storedRange)
			rangeStore_ppcRanges.deleteRange(r.storedRange);
		r.storedRange = nullptr;
	}
	// todo - free x86 code
}

void PPCRecompiler_invalidateRange(uint32 startAddr, uint32 endAddr)
{
	if (ppcRecompilerEnabled == false)
		return;
	if (startAddr >= PPC_REC_CODE_AREA_SIZE)
		return;
	if (startAddr <= 0x0675FFFFu && endAddr >= 0x06750000u)
		cemuLog_log(LogType::Force, "[INVALIDATE-0675] start=0x{:08X} end=0x{:08X}", startAddr, endAddr);
	cemu_assert_debug(endAddr >= startAddr);

	PPCRecompilerState.recompilerSpinlock.lock();

	uint32 rStart;
	uint32 rEnd;
	PPCRecFunction_t* rFunc;

	// mark range as unvisited
	for (uint64 currentAddr = (uint64)startAddr&~3; currentAddr < (uint64)(endAddr&~3); currentAddr += 4)
		ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[currentAddr / 4] = PPCRecompiler_leaveRecompilerCode_unvisited;

	// add entry to invalidation queue
	PPCRecompilerState.invalidationRanges.emplace_back(startAddr, endAddr-startAddr);


	while (rangeStore_ppcRanges.findFirstRange(startAddr, endAddr, rStart, rEnd, rFunc) )
	{
		PPCRecompiler_deleteFunction(rFunc);
	}

	PPCRecompilerState.recompilerSpinlock.unlock();
}

#if defined(ARCH_X86_64)
void PPCRecompiler_initPlatform()
{
	ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom[0] = 1ULL << 63ULL;
	ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom[1] = 0ULL;
	ppcRecompilerInstanceData->_x64XMM_xorNegateMaskPair[0] = 1ULL << 63ULL;
	ppcRecompilerInstanceData->_x64XMM_xorNegateMaskPair[1] = 1ULL << 63ULL;
	ppcRecompilerInstanceData->_x64XMM_xorNOTMask[0] = 0xFFFFFFFFFFFFFFFFULL;
	ppcRecompilerInstanceData->_x64XMM_xorNOTMask[1] = 0xFFFFFFFFFFFFFFFFULL;
	ppcRecompilerInstanceData->_x64XMM_andAbsMaskBottom[0] = ~(1ULL << 63ULL);
	ppcRecompilerInstanceData->_x64XMM_andAbsMaskBottom[1] = ~0ULL;
	ppcRecompilerInstanceData->_x64XMM_andAbsMaskPair[0] = ~(1ULL << 63ULL);
	ppcRecompilerInstanceData->_x64XMM_andAbsMaskPair[1] = ~(1ULL << 63ULL);
	ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[0] = ~(1 << 31);
	ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[1] = 0xFFFFFFFF;
	ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[2] = 0xFFFFFFFF;
	ppcRecompilerInstanceData->_x64XMM_andFloatAbsMaskBottom[3] = 0xFFFFFFFF;
	ppcRecompilerInstanceData->_x64XMM_singleWordMask[0] = 0xFFFFFFFFULL;
	ppcRecompilerInstanceData->_x64XMM_singleWordMask[1] = 0ULL;
	ppcRecompilerInstanceData->_x64XMM_constDouble1_1[0] = 1.0;
	ppcRecompilerInstanceData->_x64XMM_constDouble1_1[1] = 1.0;
	ppcRecompilerInstanceData->_x64XMM_constDouble0_0[0] = 0.0;
	ppcRecompilerInstanceData->_x64XMM_constDouble0_0[1] = 0.0;
	ppcRecompilerInstanceData->_x64XMM_constFloat0_0[0] = 0.0f;
	ppcRecompilerInstanceData->_x64XMM_constFloat0_0[1] = 0.0f;
	ppcRecompilerInstanceData->_x64XMM_constFloat1_1[0] = 1.0f;
	ppcRecompilerInstanceData->_x64XMM_constFloat1_1[1] = 1.0f;
	*(uint32*)&ppcRecompilerInstanceData->_x64XMM_constFloatMin[0] = 0x00800000;
	*(uint32*)&ppcRecompilerInstanceData->_x64XMM_constFloatMin[1] = 0x00800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[0] = 0x7F800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[1] = 0x7F800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[2] = 0x7F800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMask1[3] = 0x7F800000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[0] = ~0x80000000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[1] = ~0x80000000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[2] = ~0x80000000;
	ppcRecompilerInstanceData->_x64XMM_flushDenormalMaskResetSignBits[3] = ~0x80000000;

	// mxcsr
	ppcRecompilerInstanceData->_x64XMM_mxCsr_ftzOn = 0x1F80 | 0x8000;
	ppcRecompilerInstanceData->_x64XMM_mxCsr_ftzOff = 0x1F80;
}
#else
void PPCRecompiler_initPlatform()
{
    
}
#endif

void PPCRecompiler_init()
{
	if (ActiveSettings::GetCPUMode() == CPUMode::SinglecoreInterpreter)
	{
		ppcRecompilerEnabled = false;
		return;
	}
	if (LaunchSettings::ForceInterpreter() || LaunchSettings::ForceMultiCoreInterpreter())
	{
		cemuLog_log(LogType::Force, "Recompiler disabled. Command line --force-interpreter or force-multicore-interpreter was passed");
		return;
	}
	if (ppcRecompilerInstanceData)
	{
		MemMapper::FreeReservation(ppcRecompilerInstanceData, sizeof(PPCRecompilerInstanceData_t));
		ppcRecompilerInstanceData = nullptr;
	}
	debug_printf("Allocating %dMB for recompiler instance data...\n", (sint32)(sizeof(PPCRecompilerInstanceData_t) / 1024 / 1024));
	ppcRecompilerInstanceData = (PPCRecompilerInstanceData_t*)MemMapper::ReserveMemory(nullptr, sizeof(PPCRecompilerInstanceData_t), MemMapper::PAGE_PERMISSION::P_RW);
	MemMapper::AllocateMemory(&(ppcRecompilerInstanceData->_x64XMM_xorNegateMaskBottom), sizeof(PPCRecompilerInstanceData_t) - offsetof(PPCRecompilerInstanceData_t, _x64XMM_xorNegateMaskBottom), MemMapper::PAGE_PERMISSION::P_RW, true);
#ifdef ARCH_X86_64
	PPCRecompilerX64Gen_generateRecompilerInterfaceFunctions();
#elif defined(__aarch64__)
	PPCRecompilerAArch64Gen_generateRecompilerInterfaceFunctions();
#endif
    PPCRecompiler_allocateRange(0, 0x1000); // the first entry is used for fallback to interpreter
    PPCRecompiler_allocateRange(mmuRange_TRAMPOLINE_AREA.getBase(), mmuRange_TRAMPOLINE_AREA.getSize());
    PPCRecompiler_allocateRange(mmuRange_CODECAVE.getBase(), mmuRange_CODECAVE.getSize());

    PPCRecompiler_initPlatform();
    
	cemuLog_log(LogType::Force, "Recompiler initialized");

	ppcRecompilerEnabled = true;

	// launch recompilation thread
    s_recompilerThreadStopSignal = false;
    s_threadRecompiler = std::thread(PPCRecompiler_thread);
}

void PPCRecompiler_Shutdown()
{
    // shut down recompiler thread
    s_recompilerThreadStopSignal = true;
    if(s_threadRecompiler.joinable())
        s_threadRecompiler.join();
    // clean up queues
    while(!PPCRecompilerState.targetQueue.empty())
        PPCRecompilerState.targetQueue.pop();
    while(!PPCRecompilerState.targetQueueHigh.empty())
        PPCRecompilerState.targetQueueHigh.pop();
    PPCRecompilerState.invalidationRanges.clear();
    // clean range store
    rangeStore_ppcRanges.clear();
    // clean up memory
    uint32 numBlocks = PPCRecompiler_GetNumAddressSpaceBlocks();
    for(uint32 i=0; i<numBlocks; i++)
    {
        if(!ppcRecompiler_reservedBlockMask[i])
            continue;
        // deallocate
        uint64 offset = i * PPC_REC_ALLOC_BLOCK_SIZE;
        MemMapper::FreeMemory(&(ppcRecompilerInstanceData->ppcRecompilerFuncTable[offset/4]), (PPC_REC_ALLOC_BLOCK_SIZE/4)*sizeof(void*), true);
        MemMapper::FreeMemory(&(ppcRecompilerInstanceData->ppcRecompilerDirectJumpTable[offset/4]), (PPC_REC_ALLOC_BLOCK_SIZE/4)*sizeof(void*), true);
        // mark as unmapped
        ppcRecompiler_reservedBlockMask[i] = false;
    }
}
