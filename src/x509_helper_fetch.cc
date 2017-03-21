/**
 * This file is part of the CernVM File System.
 */

#include "x509_helper_fetch.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <cassert>
#include <climits>
#include <cstdio>
#include <cstring>

#include "x509_helper_log.h"

using namespace std;  // NOLINT

/**
 * For a given pid, extracts the X509_USER_PROXY path from the foreign
 * process' environment.  Stores the resulting path in the user provided buffer
 * path.
 */
static bool GetProxyFileFromEnv(
  const pid_t pid,
  const size_t path_len,
  char *path)
{
  assert(path_len > 0);
  static const char * const X509_USER_PROXY = "\0X509_USER_PROXY=";
  size_t X509_USER_PROXY_LEN = strlen(X509_USER_PROXY + 1) + 1;

  if (snprintf(path, path_len, "/proc/%d/environ", pid) >=
      static_cast<int>(path_len))
  {
    if (errno == 0) {errno = ERANGE;}
    return false;
  }
  int olduid = geteuid();
  // NOTE: we ignore return values of these syscalls; this code path
  // will work if cvmfs is FUSE-mounted as an unprivileged user.
  seteuid(0);

  FILE *fp = fopen(path, "r");
  if (!fp) {
    LogAuthz(kLogAuthzSyslogErr | kLogAuthzDebug,
             "failed to open environment file for pid %d.", pid);
    seteuid(olduid);
    return false;
  }

  // Look for X509_USER_PROXY in the environment and store the value in path
  int c = '\0';
  size_t idx = 0, key_idx = 0;
  bool set_env = false;
  while (1) {
    if (c == EOF) {break;}
    if (key_idx == X509_USER_PROXY_LEN) {
      if (idx >= path_len - 1) {break;}
      if (c == '\0') {set_env = true; break;}
      path[idx++] = c;
    } else if (X509_USER_PROXY[key_idx++] != c) {
      key_idx = 0;
    }
    c = fgetc(fp);
  }
  fclose(fp);
  seteuid(olduid);

  if (set_env) {path[idx] = '\0';}
  return set_env;
}


/**
 * Opens a read-only file pointer to the proxy certificate as a given user.
 * The path is either taken from X509_USER_PROXY environment from the given pid
 * or it is the default location /tmp/x509up_u<UID>
 */
static FILE *GetProxyFileInternal(pid_t pid, uid_t uid, gid_t gid)
{
  char path[PATH_MAX];
  if (!GetProxyFileFromEnv(pid, PATH_MAX, path)) {
    LogAuthz(kLogAuthzDebug,
             "could not find proxy in environment; using default location "
             "in /tmp/x509up_u%d.", uid);
    if (snprintf(path, PATH_MAX, "/tmp/x509up_u%d", uid) >= PATH_MAX) {
      if (errno == 0) {errno = ERANGE;}
      return NULL;
    }
  }
  LogAuthz(kLogAuthzDebug, "looking for proxy in file %s", path);

  /**
   * If the target process is running inside a container, then we must
   * adjust our fopen below for a chroot.
   */
  char container_path[PATH_MAX];
  if (snprintf(container_path, PATH_MAX, "/proc/%d/root", pid) >=
      PATH_MAX) {
    if (errno == 0) {errno = ERANGE;}
    return NULL;
  }
  char container_cwd[PATH_MAX];
  if (snprintf(container_cwd, PATH_MAX, "/proc/%d/cwd", pid) >=
      PATH_MAX) {
    if (errno == 0) {errno = ERANGE;}
    return NULL;
  }

  int olduid = geteuid();
  int oldgid = getegid();
  // NOTE the sequencing: we must be eUID 0
  // to change the UID and GID.
  seteuid(0);

  int fd = open("/", O_RDONLY);  // Open FD to old root directory.
  int fd2 = open(".", O_RDONLY); // Open FD to old $CWD
  if ((fd == -1) || (fd2 == -1)) {
    seteuid(olduid);
    return NULL;
  }

  // If we can't chroot, we might be running this binary unprivileged -
  // don't try subsequent changes.
  bool can_chroot = true;
  if (-1 == chdir(container_cwd)) { // Change directory to same one as process.
    can_chroot = false;
  }
  if (can_chroot && (-1 == chroot(container_path))) {
    if (-1 == fchdir(fd)) {
      // Unable to restore original state!  Abort...
      abort();
    }
    can_chroot = false;
    seteuid(olduid);
    return NULL;
  }

  setegid(gid);
  seteuid(uid);

  FILE *fp = fopen(path, "r");

  seteuid(0); // Restore root privileges.
  if (can_chroot &&
       ((-1 == fchdir(fd)) || // Change to old root directory so we can reset chroot.
        (-1 == chroot(".")) ||
        (-1 == fchdir(fd2)) // Change to original $CWD
       )
     ) {
    abort();
  }
  setegid(oldgid); // Restore remaining privileges.
  seteuid(olduid);

  return fp;
}


FILE *GetX509Proxy(
const AuthzRequest &authz_req, string *proxy) {
  assert(proxy != NULL);

  FILE *fproxy =
    GetProxyFileInternal(authz_req.pid, authz_req.uid, authz_req.gid);
  if (fproxy == NULL) {
    LogAuthz(kLogAuthzDebug, "no proxy found for %s",
             authz_req.Ident().c_str());
    return NULL;
  }

  proxy->clear();
  const unsigned kBufSize = 1024;
  char buf[kBufSize];
  unsigned nbytes;
  do {
    nbytes = fread(buf, 1, kBufSize, fproxy);
    if (nbytes > 0)
      proxy->append(string(buf, nbytes));
  } while (nbytes == kBufSize);

  rewind(fproxy);
  return fproxy;
}
