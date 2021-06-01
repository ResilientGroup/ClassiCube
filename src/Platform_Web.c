#include "Core.h"
#if defined CC_BUILD_WEB

#include "_PlatformBase.h"
#include "Stream.h"
#include "ExtMath.h"
#include "Drawer2D.h"
#include "Funcs.h"
#include "Window.h"
#include "Utils.h"
#include "Errors.h"

#include <errno.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <stdio.h>

/* Unfortunately, errno constants are different in some older emscripten versions */
/*  (linux errno compared to WASI errno) */
/* So just use the same numbers as interop_web.js (otherwise connecting always fail) */
#define _EINPROGRESS  26
#define _EAGAIN        6 /* same as EWOULDBLOCK */
#define _EHOSTUNREACH 23

const cc_result ReturnCode_FileShareViolation = 1000000000; /* TODO: not used apparently */
const cc_result ReturnCode_FileNotFound     = ENOENT;
const cc_result ReturnCode_SocketInProgess  = _EINPROGRESS;
const cc_result ReturnCode_SocketWouldBlock = _EAGAIN;
const cc_result ReturnCode_DirectoryExists  = EEXIST;
#include <emscripten.h>
#include "Chat.h"


/*########################################################################################################################*
*---------------------------------------------------------Memory----------------------------------------------------------*
*#########################################################################################################################*/
void Mem_Set(void*  dst, cc_uint8 value,  cc_uint32 numBytes) { memset(dst, value, numBytes); }
void Mem_Copy(void* dst, const void* src, cc_uint32 numBytes) { memcpy(dst, src,   numBytes); }

void* Mem_TryAlloc(cc_uint32 numElems, cc_uint32 elemsSize) {
	cc_uint32 size = CalcMemSize(numElems, elemsSize);
	return size ? malloc(size) : NULL;
}

void* Mem_TryAllocCleared(cc_uint32 numElems, cc_uint32 elemsSize) {
	return calloc(numElems, elemsSize);
}

void* Mem_TryRealloc(void* mem, cc_uint32 numElems, cc_uint32 elemsSize) {
	cc_uint32 size = CalcMemSize(numElems, elemsSize);
	return size ? realloc(mem, size) : NULL;
}

void Mem_Free(void* mem) {
	if (mem) free(mem);
}


/*########################################################################################################################*
*------------------------------------------------------Logging/Time-------------------------------------------------------*
*#########################################################################################################################*/
/* TODO: check this is actually accurate */
static cc_uint64 sw_freqMul = 1, sw_freqDiv = 1;
cc_uint64 Stopwatch_ElapsedMicroseconds(cc_uint64 beg, cc_uint64 end) {
	if (end < beg) return 0;
	return ((end - beg) * sw_freqMul) / sw_freqDiv;
}

extern void interop_Log(const char* msg, int len);
void Platform_Log(const char* msg, int len) {
	interop_Log(msg, len);
}

#define UnixTime_TotalMS(time) ((cc_uint64)time.tv_sec * 1000 + UNIX_EPOCH + (time.tv_usec / 1000))
TimeMS DateTime_CurrentUTC_MS(void) {
	struct timeval cur;
	gettimeofday(&cur, NULL);
	return UnixTime_TotalMS(cur);
}

void DateTime_CurrentLocal(struct DateTime* t) {
	struct timeval cur; 
	struct tm loc_time;
	gettimeofday(&cur, NULL);
	localtime_r(&cur.tv_sec, &loc_time);

	t->year   = loc_time.tm_year + 1900;
	t->month  = loc_time.tm_mon  + 1;
	t->day    = loc_time.tm_mday;
	t->hour   = loc_time.tm_hour;
	t->minute = loc_time.tm_min;
	t->second = loc_time.tm_sec;
}

cc_uint64 Stopwatch_Measure(void) {
	/* time is a milliseconds double */
	return (cc_uint64)(emscripten_get_now() * 1000);
}


/*########################################################################################################################*
*-----------------------------------------------------Directory/File------------------------------------------------------*
*#########################################################################################################################*/
cc_result Directory_Create(const cc_string* path) {
	char str[NATIVE_STR_LEN];
	Platform_EncodeUtf8(str, path);
	/* read/write/search permissions for owner and group, and with read/search permissions for others. */
	/* TODO: Is the default mode in all cases */
	return mkdir(str, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH) == -1 ? errno : 0;
}

int File_Exists(const cc_string* path) {
	char str[NATIVE_STR_LEN];
	struct stat sb;
	Platform_EncodeUtf8(str, path);
	return stat(str, &sb) == 0 && S_ISREG(sb.st_mode);
}

cc_result Directory_Enum(const cc_string* dirPath, void* obj, Directory_EnumCallback callback) {
	cc_string path; char pathBuffer[FILENAME_SIZE];
	char str[NATIVE_STR_LEN];
	DIR* dirPtr;
	struct dirent* entry;
	char* src;
	int len, res;

	Platform_EncodeUtf8(str, dirPath);
	dirPtr = opendir(str);
	if (!dirPtr) return errno;

	/* POSIX docs: "When the end of the directory is encountered, a null pointer is returned and errno is not changed." */
	/* errno is sometimes leftover from previous calls, so always reset it before readdir gets called */
	errno = 0;
	String_InitArray(path, pathBuffer);

	while ((entry = readdir(dirPtr))) {
		path.length = 0;
		String_Format1(&path, "%s/", dirPath);

		/* ignore . and .. entry */
		src = entry->d_name;
		if (src[0] == '.' && src[1] == '\0') continue;
		if (src[0] == '.' && src[1] == '.' && src[2] == '\0') continue;

		len = String_Length(src);
		String_AppendUtf8(&path, src, len);

		/* TODO: fallback to stat when this fails */
		if (entry->d_type == DT_DIR) {
			res = Directory_Enum(&path, obj, callback);
			if (res) { closedir(dirPtr); return res; }
		} else {
			callback(&path, obj);
		}
		errno = 0;
	}

	res = errno; /* return code from readdir */
	closedir(dirPtr);
	return res;
}

static cc_result File_Do(cc_file* file, const cc_string* path, int mode) {
	char str[NATIVE_STR_LEN];
	Platform_EncodeUtf8(str, path);
	*file = open(str, mode, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	return *file == -1 ? errno : 0;
}

cc_result File_Open(cc_file* file, const cc_string* path) {
	return File_Do(file, path, O_RDONLY);
}
cc_result File_Create(cc_file* file, const cc_string* path) {
	return File_Do(file, path, O_RDWR | O_CREAT | O_TRUNC);
}
cc_result File_OpenOrCreate(cc_file* file, const cc_string* path) {
	return File_Do(file, path, O_RDWR | O_CREAT);
}

cc_result File_Read(cc_file file, void* data, cc_uint32 count, cc_uint32* bytesRead) {
	*bytesRead = read(file, data, count);
	return *bytesRead == -1 ? errno : 0;
}

cc_result File_Write(cc_file file, const void* data, cc_uint32 count, cc_uint32* bytesWrote) {
	*bytesWrote = write(file, data, count);
	return *bytesWrote == -1 ? errno : 0;
}

extern void interop_SyncFS(void);
cc_result File_Close(cc_file file) {	
	int res = close(file) == -1 ? errno : 0; 
	interop_SyncFS(); 
	return res;
}

cc_result File_Seek(cc_file file, int offset, int seekType) {
	static cc_uint8 modes[3] = { SEEK_SET, SEEK_CUR, SEEK_END };
	return lseek(file, offset, modes[seekType]) == -1 ? errno : 0;
}

cc_result File_Position(cc_file file, cc_uint32* pos) {
	*pos = lseek(file, 0, SEEK_CUR);
	return *pos == -1 ? errno : 0;
}

cc_result File_Length(cc_file file, cc_uint32* len) {
	struct stat st;
	if (fstat(file, &st) == -1) { *len = -1; return errno; }
	*len = st.st_size; return 0;
}


/*########################################################################################################################*
*--------------------------------------------------------Threading--------------------------------------------------------*
*#########################################################################################################################*/
/* No real threading support with emscripten backend */
void Thread_Sleep(cc_uint32 milliseconds) { }
void* Thread_Start(Thread_StartFunc func) { func(); return NULL; }
void Thread_Detach(void* handle) { }
void Thread_Join(void* handle) { }

void* Mutex_Create(void) { return NULL; }
void Mutex_Free(void* handle) { }
void Mutex_Lock(void* handle) { }
void Mutex_Unlock(void* handle) { }

void* Waitable_Create(void) { return NULL; }
void Waitable_Free(void* handle) { }
void Waitable_Signal(void* handle) { }
void Waitable_Wait(void* handle) { }
void Waitable_WaitFor(void* handle, cc_uint32 milliseconds) { }


/*########################################################################################################################*
*--------------------------------------------------------Font/Text--------------------------------------------------------*
*#########################################################################################################################*/
void Platform_LoadSysFonts(void) { }


/*########################################################################################################################*
*---------------------------------------------------------Socket----------------------------------------------------------*
*#########################################################################################################################*/
extern int interop_SocketCreate(void);
extern int interop_SocketConnect(int sock, const char* addr, int port);
extern int interop_SocketClose(int sock);
extern int interop_SocketSend(int sock, const void* data, int len);
extern int interop_SocketRecv(int sock, void* data, int len);
extern int interop_SocketGetPending(int sock);
extern int interop_SocketGetError(int sock);
extern int interop_SocketPoll(int sock);

cc_result Socket_Create(cc_socket* s) {
	*s = interop_SocketCreate();
	return 0;
}

cc_result Socket_Available(cc_socket s, int* available) {
	int res = interop_SocketGetPending(s);
	/* returned result is negative for error */

	if (res >= 0) {
		*available = res; return 0;
	} else {
		*available = 0; return -res;
	}
}

cc_result Socket_GetError(cc_socket s, cc_result* result) {
	int res = interop_SocketGetError(s);
	/* returned result is negative for error */

	if (res >= 0) {
		*result = res; return 0;
	} else {
		*result = 0; return -res;
	}
}

cc_result Socket_Connect(cc_socket s, const cc_string* ip, int port) {
	char addr[NATIVE_STR_LEN];
	int res;
	Platform_EncodeUtf8(addr, ip);
	/* returned result is negative for error */
	res = -interop_SocketConnect(s, addr, port);

	/* error returned when invalid address provided */
	if (res == _EHOSTUNREACH) return ERR_INVALID_ARGUMENT;
	return res;
}

cc_result Socket_Read(cc_socket s, cc_uint8* data, cc_uint32 count, cc_uint32* modified) {
	/* recv only reads one WebSocket frame at most, hence call it multiple times */
	int res; *modified = 0;

	while (count && interop_SocketGetPending(s) > 0) {
		/* returned result is negative for error */
		res = interop_SocketRecv(s, data, count);

		if (res >= 0) {
			*modified += res;
			data      += res; count -= res;
		} else {
			/* EAGAIN when no data available */
			if (res == -_EAGAIN) break;
			return -res;
		}
	}
	return 0;
}

cc_result Socket_Write(cc_socket s, const cc_uint8* data, cc_uint32 count, cc_uint32* modified) {
	/* returned result is negative for error */
	int res = interop_SocketSend(s, data, count);

	if (res >= 0) {
		*modified = res; return 0;
	} else {
		*modified = 0; return -res;
	}
}

cc_result Socket_Close(cc_socket s) {
	/* returned result is negative for error */
	return -interop_SocketClose(s);
}

cc_result Socket_Poll(cc_socket s, int mode, cc_bool* success) {
	/* returned result is negative for error */
	int res = interop_SocketPoll(s), flags;

	if (res >= 0) {
		flags    = mode == SOCKET_POLL_READ ? 0x01 : 0x02;
		*success = (res & flags) != 0;
		return 0;
	} else {
		*success = false; return -res;
	}
}


/*########################################################################################################################*
*-----------------------------------------------------Process/Module------------------------------------------------------*
*#########################################################################################################################*/
cc_result Process_StartGame(const cc_string* args) { return ERR_NOT_SUPPORTED; }
void Process_Exit(cc_result code) { exit(code); }

extern int interop_OpenTab(const char* url);
cc_result Process_StartOpen(const cc_string* args) {
	char str[NATIVE_STR_LEN];
	Platform_EncodeUtf8(str, args);
	return interop_OpenTab(str);
}


/*########################################################################################################################*
*--------------------------------------------------------Updater----------------------------------------------------------*
*#########################################################################################################################*/
const char* const Updater_D3D9 = NULL;
const char* const Updater_OGL  = NULL;

cc_result Updater_GetBuildTime(cc_uint64* t)   { return ERR_NOT_SUPPORTED; }
cc_bool Updater_Clean(void)                    { return true; }
cc_result Updater_Start(const char** action)   { return ERR_NOT_SUPPORTED; }
cc_result Updater_MarkExecutable(void)         { return 0; }
cc_result Updater_SetNewBuildTime(cc_uint64 t) { return ERR_NOT_SUPPORTED; }


/*########################################################################################################################*
*-------------------------------------------------------Dynamic lib-------------------------------------------------------*
*#########################################################################################################################*/
void* DynamicLib_Load2(const cc_string* path)      { return NULL; }
void* DynamicLib_Get2(void* lib, const char* name) { return NULL; }
cc_bool DynamicLib_DescribeError(cc_string* dst)   { return false; }


/*########################################################################################################################*
*--------------------------------------------------------Platform---------------------------------------------------------*
*#########################################################################################################################*/
int Platform_EncodeUtf8(void* data, const cc_string* src) {
	cc_uint8* dst = (cc_uint8*)data;
	cc_uint8* cur;
	int i, len = 0;
	if (src->length > FILENAME_SIZE) Logger_Abort("String too long to expand");

	for (i = 0; i < src->length; i++) {
		cur = dst + len;
		len += Convert_CP437ToUtf8(src->buffer[i], cur);
	}
	dst[len] = '\0';
	return len;
}

cc_bool Platform_DescribeError(cc_result res, cc_string* dst) {
	char* str;
	int len;

	/* For unrecognised error codes, strerror might return messages */
	/*  such as 'No error information', which is not very useful */
	if (res >= 1000) return false;

	str = strerror(res);
	if (!str) return false;

	len = String_CalcLen(str, NATIVE_STR_LEN);
	String_AppendUtf8(dst, str, len);
	return true;
}

EMSCRIPTEN_KEEPALIVE void Platform_LogError(const char* msg) {
	/* no point showing more than 128 characters in chat */
	cc_string str = String_FromRaw(msg, 128);
	Logger_WarnFunc(&str);
}

extern void interop_InitModule(void);
extern void interop_InitSockets(void);
extern void interop_GetIndexedDBError(char* buffer);

void Platform_Init(void) {
	char tmp[64+1] = { 0 };
	interop_InitModule();
	interop_InitSockets();
	
	/* Check if an error occurred when pre-loading IndexedDB */
	interop_GetIndexedDBError(tmp);
	if (!tmp[0]) return;
	
	Chat_Add1("&cError preloading IndexedDB: %c", tmp);
	Chat_AddRaw("&cPreviously saved settings/maps will be lost");
	/* NOTE: You must pre-load IndexedDB before main() */
	/* (because pre-loading only works asynchronously) */
	/* If you don't, you'll get errors later trying to sync local to remote */
	/* See doc/hosting-webclient.md for example preloading IndexedDB code */
}
void Platform_Free(void) { }


/*########################################################################################################################*
*-------------------------------------------------------Encryption--------------------------------------------------------*
*#########################################################################################################################*/
cc_result Platform_Encrypt(const void* data, int len, cc_string* dst) { return ERR_NOT_SUPPORTED; }
cc_result Platform_Decrypt(const void* data, int len, cc_string* dst) { return ERR_NOT_SUPPORTED; }


/*########################################################################################################################*
*-----------------------------------------------------Configuration-------------------------------------------------------*
*#########################################################################################################################*/
int Platform_GetCommandLineArgs(int argc, STRING_REF char** argv, cc_string* args) {
	int i, count;
	argc--; argv++; /* skip executable path argument */

	count = min(argc, GAME_MAX_CMDARGS);
	for (i = 0; i < count; i++) { args[i] = String_FromReadonly(argv[i]); }
	return count;
}

cc_result Platform_SetDefaultCurrentDirectory(int argc, char **argv) {
	return chdir("/classicube") == -1 ? errno : 0;
}
#endif
