#include "sqlite_utils.hpp"

#ifdef __SWITCH__

#include <switch.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>

namespace aurora::sqlite {

namespace {
int sLastOpenErrno = 0;
} // namespace

int last_switch_open_errno() { return sLastOpenErrno; }

namespace {

struct SwitchFile {
  sqlite3_io_methods const* pMethods = nullptr;
  FILE* handle = nullptr;
  bool deleteOnClose = false;
  char name[1024] = {};
};

int switchClose(sqlite3_file* file) {
  auto* f = reinterpret_cast<SwitchFile*>(file);
  int rc = SQLITE_OK;
  if (f->handle != nullptr && fclose(f->handle) != 0) {
    rc = SQLITE_IOERR_CLOSE;
  }
  f->handle = nullptr;
  if (f->deleteOnClose) {
    remove(f->name);
  }
  return rc;
}

int switchRead(sqlite3_file* file, void* buf, int amt, sqlite3_int64 offset) {
  auto* f = reinterpret_cast<SwitchFile*>(file);
  if (fseek(f->handle, static_cast<long>(offset), SEEK_SET) != 0) {
    return SQLITE_IOERR_READ;
  }
  const size_t want = static_cast<size_t>(amt);
  const size_t got = fread(buf, 1, want, f->handle);
  if (got == want) {
    return SQLITE_OK;
  }
  if (ferror(f->handle) == 0) {
    return SQLITE_IOERR_SHORT_READ;
  }

  clearerr(f->handle);
  if (fseek(f->handle, 0, SEEK_END) != 0) {
    return SQLITE_IOERR_READ;
  }
  const long size = ftell(f->handle);
  if (size >= 0 && offset >= static_cast<sqlite3_int64>(size)) {
    return SQLITE_IOERR_SHORT_READ;
  }
  return SQLITE_IOERR_READ;
}

int switchWrite(sqlite3_file* file, const void* buf, int amt, sqlite3_int64 offset) {
  auto* f = reinterpret_cast<SwitchFile*>(file);
  if (fseek(f->handle, static_cast<long>(offset), SEEK_SET) != 0) {
    return SQLITE_IOERR_WRITE;
  }
  const size_t wrote = fwrite(buf, 1, static_cast<size_t>(amt), f->handle);
  return wrote == static_cast<size_t>(amt) ? SQLITE_OK : SQLITE_IOERR_WRITE;
}

int switchTruncate(sqlite3_file* file, sqlite3_int64 size) {
  auto* f = reinterpret_cast<SwitchFile*>(file);
  if (fseek(f->handle, 0, SEEK_END) != 0) {
    return SQLITE_IOERR_TRUNCATE;
  }
  const long current = ftell(f->handle);
  if (current < 0) {
    return SQLITE_IOERR_TRUNCATE;
  }
  if (static_cast<sqlite3_int64>(current) == size) {
    return SQLITE_OK;
  }
  if (ftruncate(fileno(f->handle), static_cast<off_t>(size)) != 0) {
    return SQLITE_IOERR_TRUNCATE;
  }
  return SQLITE_OK;
}

int switchSync(sqlite3_file* file, int) {
  auto* f = reinterpret_cast<SwitchFile*>(file);
  return fflush(f->handle) == 0 ? SQLITE_OK : SQLITE_IOERR_FSYNC;
}

int switchFileSize(sqlite3_file* file, sqlite3_int64* size) {
  auto* f = reinterpret_cast<SwitchFile*>(file);
  if (fseek(f->handle, 0, SEEK_END) != 0) {
    return SQLITE_IOERR_FSTAT;
  }
  const long pos = ftell(f->handle);
  if (pos < 0) {
    return SQLITE_IOERR_FSTAT;
  }
  *size = static_cast<sqlite3_int64>(pos);
  return SQLITE_OK;
}

int switchLock(sqlite3_file*, int) { return SQLITE_OK; }
int switchUnlock(sqlite3_file*, int) { return SQLITE_OK; }
int switchCheckReservedLock(sqlite3_file* file, int* res) {
  (void)file;
  *res = 0;
  return SQLITE_OK;
}

int switchFileControl(sqlite3_file*, int op, void* arg) {
  if (op == SQLITE_FCNTL_MMAP_SIZE && arg != nullptr) {
    *static_cast<sqlite3_int64*>(arg) = 0;
    return SQLITE_OK;
  }
  return SQLITE_NOTFOUND;
}

int switchSectorSize(sqlite3_file*) { return 4096; }
int switchDeviceCharacteristics(sqlite3_file*) { return 0; }

constexpr sqlite3_io_methods kSwitchIoMethods = {
    1,
    switchClose,
    switchRead,
    switchWrite,
    switchTruncate,
    switchSync,
    switchFileSize,
    switchLock,
    switchUnlock,
    switchCheckReservedLock,
    switchFileControl,
    switchSectorSize,
    switchDeviceCharacteristics,
};

int switchOpen(sqlite3_vfs*, const char* name, sqlite3_file* file, int flags, int* outFlags) {
  auto* f = reinterpret_cast<SwitchFile*>(file);
  if (name == nullptr) {
    return SQLITE_CANTOPEN;
  }
  f->pMethods = &kSwitchIoMethods;
  f->deleteOnClose = (flags & SQLITE_OPEN_DELETEONCLOSE) != 0;
  strncpy(f->name, name, sizeof(f->name) - 1);

  const bool readonly = (flags & SQLITE_OPEN_READONLY) != 0;
  const bool create = (flags & SQLITE_OPEN_CREATE) != 0;
  FILE* handle = nullptr;
  if (readonly) {
    handle = fopen(name, "rb");
  } else {
    handle = fopen(name, "r+b");
    if (handle == nullptr && create) {
      handle = fopen(name, "w+b");
    }
  }
  if (handle == nullptr) {
    sLastOpenErrno = errno;
    return SQLITE_CANTOPEN;
  }
  sLastOpenErrno = 0;
  f->handle = handle;
  if (outFlags != nullptr) {
    *outFlags = readonly ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;
  }
  return SQLITE_OK;
}

int switchDelete(sqlite3_vfs*, const char* name, int syncDir) {
  (void)syncDir;
  if (name == nullptr) {
    return SQLITE_IOERR_DELETE;
  }
  if (remove(name) != 0 && errno != ENOENT) {
    return SQLITE_IOERR_DELETE;
  }
  return SQLITE_OK;
}

int switchAccess(sqlite3_vfs*, const char* name, int, int* res) {
  if (name == nullptr) {
    *res = 0;
    return SQLITE_OK;
  }
  FILE* probe = fopen(name, "rb");
  *res = (probe != nullptr) ? 1 : 0;
  if (probe != nullptr) {
    fclose(probe);
  }
  return SQLITE_OK;
}

int switchFullPathname(sqlite3_vfs* vfs, const char* name, int nOut, char* out) {
  (void)vfs;
  const size_t len = strlen(name);
  if (nOut < 0 || static_cast<size_t>(nOut) < len + 1) {
    return SQLITE_CANTOPEN;
  }
  memcpy(out, name, len + 1);
  if (static_cast<size_t>(nOut) > len + 1) {
    out[len + 1] = 0;
  }
  if (static_cast<size_t>(nOut) > len + 2) {
    out[len + 2] = 0;
  }
  return SQLITE_OK;
}

int switchRandomness(sqlite3_vfs*, int nBytes, char* out) {
  if (nBytes <= 0) {
    return 0;
  }
  randomGet(out, static_cast<size_t>(nBytes));
  return nBytes;
}

int switchSleep(sqlite3_vfs*, int microseconds) {
  svcSleepThread(static_cast<u64>(microseconds) * 1000u);
  return microseconds;
}

int switchCurrentTime(sqlite3_vfs*, double* jd) {
  const time_t now = time(nullptr);
  if (now == static_cast<time_t>(-1)) {
    return SQLITE_ERROR;
  }
  *jd = static_cast<double>(now) / 86400.0 + 2440587.5;
  return SQLITE_OK;
}

int switchCurrentTimeInt64(sqlite3_vfs*, sqlite3_int64* ms) {
  const time_t now = time(nullptr);
  if (now == static_cast<time_t>(-1)) {
    return SQLITE_ERROR;
  }
  *ms = (static_cast<sqlite3_int64>(now) - 62135596800LL) * 1000LL;
  return SQLITE_OK;
}

int switchGetLastError(sqlite3_vfs*, int nBytes, char* err) {
  snprintf(err, static_cast<size_t>(nBytes), "errno=%d", errno);
  return errno;
}

sqlite3_vfs kSwitchVfs = {
    1,                 // iVersion
    sizeof(SwitchFile),
    1024,              // mxPathname
    nullptr,           // pNext
    "switch",          // zName
    nullptr,           // pAppData
    switchOpen,
    switchDelete,
    switchAccess,
    switchFullPathname,
    nullptr,           // xDlOpen
    nullptr,           // xDlError
    nullptr,           // xDlSym
    nullptr,           // xDlClose
    switchRandomness,
    switchSleep,
    switchCurrentTime,
    switchGetLastError,
    switchCurrentTimeInt64,
};

} // namespace

void register_switch_vfs() {
  static bool registered = false;
  if (!registered) {
    if (sqlite3_vfs_find("switch") == nullptr) {
      sqlite3_vfs_register(&kSwitchVfs, 0);
    }
    registered = true;
  }
}

} // namespace aurora::sqlite

#endif // __SWITCH__
