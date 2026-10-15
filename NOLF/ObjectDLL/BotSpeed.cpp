// ----------------------------------------------------------------------- //
//
// MODULE  : BotSpeed.cpp
//
// PURPOSE : Faster-than-real-time self-play (bot training on the dedicated
//			 server).  Only used when NOLFBOT_TIMESCALE is set: the clocks the
//			 engine reads (timeGetTime / QueryPerformanceCounter) run N times
//			 faster, and its Sleep()s are N times shorter, so the server also
//			 runs N times more frames per real second and each simulated step
//			 stays as short as in a normal game.  Never active in normal play.
//
//			 Kept in its own file: it needs <windows.h>, whose macros clash
//			 with the engine headers.
//
// ----------------------------------------------------------------------- //

#include <windows.h>

static double	s_fScale = 1.0;
static DWORD	(WINAPI *s_pTimeGetTime)(void) = 0;
static BOOL		(WINAPI *s_pQPC)(LARGE_INTEGER*) = 0;
static void		(WINAPI *s_pSleep)(DWORD) = 0;
static DWORD	s_dwTimeBase = 0;
static LONGLONG	s_llQPCBase = 0;

static DWORD WINAPI Fast_timeGetTime(void)
{
	DWORD dwNow = s_pTimeGetTime();
	return s_dwTimeBase + (DWORD)((double)(DWORD)(dwNow - s_dwTimeBase) * s_fScale);
}

static BOOL WINAPI Fast_QueryPerformanceCounter(LARGE_INTEGER* pCount)
{
	BOOL bOk = s_pQPC(pCount);
	if (bOk && pCount)
	{
		pCount->QuadPart = s_llQPCBase + (LONGLONG)((double)(pCount->QuadPart - s_llQPCBase) * s_fScale);
	}
	return bOk;
}

static void WINAPI Fast_Sleep(DWORD dwMS)
{
	DWORD dw = (DWORD)(dwMS / s_fScale);
	if (dwMS && !dw) dw = 1;	// still yield the CPU now and then
	s_pSleep(dw);
}

// Frame pacing can also be a wait with a timeout.  Only finite, short
// timeouts are shortened (pure synchronisation waits are left alone).
static DWORD (WINAPI *s_pWaitOne)(HANDLE, DWORD) = 0;
static DWORD (WINAPI *s_pWaitMany)(DWORD, const HANDLE*, BOOL, DWORD) = 0;

static DWORD ScaleTimeout(DWORD dwMS)
{
	if (dwMS == 0 || dwMS == INFINITE || dwMS > 1000) return dwMS;
	DWORD dw = (DWORD)(dwMS / s_fScale);
	return dw ? dw : 1;
}

static DWORD WINAPI Fast_WaitForSingleObject(HANDLE h, DWORD dwMS)
{
	return s_pWaitOne(h, ScaleTimeout(dwMS));
}

static DWORD WINAPI Fast_WaitForMultipleObjects(DWORD n, const HANDLE* ph, BOOL bAll, DWORD dwMS)
{
	return s_pWaitMany(n, ph, bAll, ScaleTimeout(dwMS));
}

// Point one module's imports of pOld at pNew; returns how many were patched.
static int PatchImports(HMODULE hMod, void* pOld, void* pNew)
{
	if (!hMod || !pOld) return 0;

	BYTE* pBase = (BYTE*)hMod;
	IMAGE_DOS_HEADER* pDos = (IMAGE_DOS_HEADER*)pBase;
	if (pDos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
	IMAGE_NT_HEADERS* pNt = (IMAGE_NT_HEADERS*)(pBase + pDos->e_lfanew);
	if (pNt->Signature != IMAGE_NT_SIGNATURE) return 0;

	IMAGE_DATA_DIRECTORY& dir = pNt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir.VirtualAddress) return 0;

	int nPatched = 0;
	for (IMAGE_IMPORT_DESCRIPTOR* pImp = (IMAGE_IMPORT_DESCRIPTOR*)(pBase + dir.VirtualAddress); pImp->Name; pImp++)
	{
		for (IMAGE_THUNK_DATA* pThunk = (IMAGE_THUNK_DATA*)(pBase + pImp->FirstThunk); pThunk->u1.Function; pThunk++)
		{
			if ((void*)(ULONG_PTR)pThunk->u1.Function != pOld) continue;

			DWORD dwOldProt;
			if (VirtualProtect(&pThunk->u1.Function, sizeof(pThunk->u1.Function), PAGE_READWRITE, &dwOldProt))
			{
				pThunk->u1.Function = (ULONG_PTR)pNew;
				VirtualProtect(&pThunk->u1.Function, sizeof(pThunk->u1.Function), dwOldProt, &dwOldProt);
				nPatched++;
			}
		}
	}
	return nPatched;
}

// Returns the number of patched imports (0 = nothing done).  Safe to call
// more than once; only the first call does anything.
int BotInstallSelfPlayClock(double fScale)
{
	static int s_nPatched = -1;
	if (s_nPatched >= 0) return s_nPatched;
	s_nPatched = 0;
	if (fScale <= 1.0) return 0;

	HMODULE hWinmm	= GetModuleHandleA("winmm.dll");
	HMODULE hKernel	= GetModuleHandleA("kernel32.dll");
	if (!hWinmm || !hKernel) return 0;

	s_pTimeGetTime	= (DWORD (WINAPI*)(void))GetProcAddress(hWinmm, "timeGetTime");
	s_pQPC			= (BOOL (WINAPI*)(LARGE_INTEGER*))GetProcAddress(hKernel, "QueryPerformanceCounter");
	s_pSleep		= (void (WINAPI*)(DWORD))GetProcAddress(hKernel, "Sleep");
	s_pWaitOne		= (DWORD (WINAPI*)(HANDLE, DWORD))GetProcAddress(hKernel, "WaitForSingleObject");
	s_pWaitMany		= (DWORD (WINAPI*)(DWORD, const HANDLE*, BOOL, DWORD))GetProcAddress(hKernel, "WaitForMultipleObjects");
	if (!s_pTimeGetTime || !s_pQPC || !s_pSleep) return 0;

	s_fScale	 = fScale;
	s_dwTimeBase = s_pTimeGetTime();
	LARGE_INTEGER li;
	s_pQPC(&li);
	s_llQPCBase	 = li.QuadPart;

	// The engine (server.dll) and the dedicated server program around it
	HMODULE ahMods[2] = { GetModuleHandleA("server.dll"), GetModuleHandleA(NULL) };
	for (int m = 0; m < 2; m++)
	{
		s_nPatched += PatchImports(ahMods[m], (void*)s_pTimeGetTime, (void*)Fast_timeGetTime);
		s_nPatched += PatchImports(ahMods[m], (void*)s_pQPC, (void*)Fast_QueryPerformanceCounter);
		s_nPatched += PatchImports(ahMods[m], (void*)s_pSleep, (void*)Fast_Sleep);
		if (s_pWaitOne)  s_nPatched += PatchImports(ahMods[m], (void*)s_pWaitOne, (void*)Fast_WaitForSingleObject);
		if (s_pWaitMany) s_nPatched += PatchImports(ahMods[m], (void*)s_pWaitMany, (void*)Fast_WaitForMultipleObjects);
	}
	return s_nPatched;
}
