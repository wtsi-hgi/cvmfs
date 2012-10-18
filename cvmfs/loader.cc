/**
 * This file is part of the CernVM File System.
 *
 * Implements stub callback functions for Fuse.  Their purpose is to
 * redirect calls to the cvmfs shared library and to block calls during the
 * update of the library.
 *
 * The main executable and the cvmfs shared library _must not_ share any
 * symbols.
 */

#define ENOATTR ENODATA  /**< instead of including attr/xattr.h */
#define FUSE_USE_VERSION 26
#define _FILE_OFFSET_BITS 64

#include "cvmfs_config.h"
#include "loader.h"

#include <sys/resource.h>
#include <dlfcn.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <time.h>

#include <openssl/crypto.h>
#include <fuse/fuse_lowlevel.h>
#include <fuse/fuse_opt.h>

#include <cstdlib>
#include <cstring>

#include <vector>
#include <string>

#include "logging.h"
#include "options.h"
#include "util.h"
#include "atomic.h"

using namespace std;  // NOLINT

namespace loader {

// Follow the fuse convention for option parsing
struct CvmfsOptions {
  char *config;
  int uid;
  int gid;
  int grab_mountpoint;
};

enum {
  KEY_HELP,
  KEY_VERSION,
  KEY_FOREGROUND,
  KEY_SINGLETHREAD,
  KEY_DEBUG,
};
#define CVMFS_OPT(t, p, v) { t, offsetof(struct CvmfsOptions, p), v }
#define CVMFS_SWITCH(t, p) { t, offsetof(struct CvmfsOptions, p), 1 }
static struct fuse_opt cvmfs_array_opts[] = {
  CVMFS_OPT("config=%s",          config, 0),
  CVMFS_OPT("uid=%d",             uid, 0),
  CVMFS_OPT("gid=%d",             gid, 0),
  CVMFS_SWITCH("grab_mountpoint", grab_mountpoint),

  FUSE_OPT_KEY("-V",            KEY_VERSION),
  FUSE_OPT_KEY("--version",     KEY_VERSION),
  FUSE_OPT_KEY("-h",            KEY_HELP),
  FUSE_OPT_KEY("--help",        KEY_HELP),
  FUSE_OPT_KEY("-f",            KEY_FOREGROUND),
  FUSE_OPT_KEY("-d",            KEY_DEBUG),
  FUSE_OPT_KEY("debug",         KEY_DEBUG),
  FUSE_OPT_KEY("-s",            KEY_SINGLETHREAD),
  {0, 0, 0},
};


string *repository_name_ = NULL;
string *mount_point_ = NULL;
string *config_files_ = NULL;
uid_t uid_ = 0;
gid_t gid_ = 0;
bool single_threaded_ = false;
bool foreground_ = false;
bool debug_mode_ = false;
bool grab_mountpoint_ = false;
atomic_int32 blocking_;
atomic_int64 num_operations_;
void *library_handle_;
CvmfsExports *cvmfs_exports_;
LoaderExports *loader_exports_;


static void Usage(const std::string &exename) {
  LogCvmfs(kLogCvmfs, kLogStdout,
    "The CernVM File System\n"
    "Version %s\n"
    "Copyright (c) 2009- CERN, all rights reserved\n\n"
    "Please visit http://cernvm.cern.ch for details.\n\n"
    "Usage: %s [-s] [-d] [-o mount options] <repository name> <mount point>\n"
    "CernVM-FS mount options:\n"
    "  -o config=FILES      colon-separated path list of config files\n"
    "  -o uid=UID           Drop credentials to another user\n"
    "  -o gid=GID           Drop credentials to another group\n"
    "  -o grab_mountpoint   give ownership of the mountpoint to the user "
                            "before mounting (required for autofs)\n\n"
    "Fuse mount options:\n"
    "  -o allow_other       allow access to other users\n"
    "  -o allow_root        allow access to root\n"
    "  -o nonempty          allow mounts over non-empty directory\n",
    PACKAGE_VERSION, exename.c_str()
  );
}


static inline void FileSystemFence() {
  while (atomic_read32(&blocking_)) {
    // Don't sleep, interferes with alarm()
    sched_yield();
  }
}


static void stub_init(void *userdata, struct fuse_conn_info *conn) {
  FileSystemFence();
  atomic_inc64(&num_operations_);
  cvmfs_exports_->cvmfs_operations.init(userdata, conn);
  atomic_dec64(&num_operations_);
}


static void stub_destroy(void *userdata) {
  FileSystemFence();
  atomic_inc64(&num_operations_);
  cvmfs_exports_->cvmfs_operations.destroy(userdata);
  // Unmounting, don't decrease num_operations_ counter
}


static void stub_lookup(fuse_req_t req, fuse_ino_t parent,
                        const char *name)
{
  FileSystemFence();
  atomic_inc64(&num_operations_);
  cvmfs_exports_->cvmfs_operations.lookup(req, parent, name);
  atomic_dec64(&num_operations_);
}


static void stub_getattr(fuse_req_t req, fuse_ino_t ino,
                         struct fuse_file_info *fi)
{
  FileSystemFence();
  atomic_inc64(&num_operations_);
  cvmfs_exports_->cvmfs_operations.getattr(req, ino, fi);
  atomic_dec64(&num_operations_);
}


static void stub_readlink(fuse_req_t req, fuse_ino_t ino) {
  FileSystemFence();
  atomic_inc64(&num_operations_);
  cvmfs_exports_->cvmfs_operations.readlink(req, ino);
  atomic_dec64(&num_operations_);
}


static void stub_opendir(fuse_req_t req, fuse_ino_t ino,
                         struct fuse_file_info *fi)
{
  FileSystemFence();
  atomic_inc64(&num_operations_);
  cvmfs_exports_->cvmfs_operations.opendir(req, ino, fi);
  atomic_dec64(&num_operations_);
}


static void stub_releasedir(fuse_req_t req, fuse_ino_t ino,
                            struct fuse_file_info *fi)
{
  FileSystemFence();
  atomic_inc64(&num_operations_);
  cvmfs_exports_->cvmfs_operations.releasedir(req, ino, fi);
  atomic_dec64(&num_operations_);
}


static void stub_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                         off_t off, struct fuse_file_info *fi)
{
  FileSystemFence();
  atomic_inc64(&num_operations_);
  cvmfs_exports_->cvmfs_operations.readdir(req, ino, size, off, fi);
  atomic_dec64(&num_operations_);
}


static void stub_open(fuse_req_t req, fuse_ino_t ino,
                      struct fuse_file_info *fi)
{
  FileSystemFence();
  atomic_inc64(&num_operations_);
  cvmfs_exports_->cvmfs_operations.open(req, ino, fi);
  atomic_dec64(&num_operations_);
}


static void stub_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                       struct fuse_file_info *fi)
{
  FileSystemFence();
  atomic_inc64(&num_operations_);
  cvmfs_exports_->cvmfs_operations.read(req, ino, size, off, fi);
  atomic_dec64(&num_operations_);
}


static void stub_release(fuse_req_t req, fuse_ino_t ino,
                         struct fuse_file_info *fi)
{
  FileSystemFence();
  atomic_inc64(&num_operations_);
  cvmfs_exports_->cvmfs_operations.release(req, ino, fi);
  atomic_dec64(&num_operations_);
}


static void stub_statfs(fuse_req_t req, fuse_ino_t ino) {
  FileSystemFence();
  atomic_inc64(&num_operations_);
  cvmfs_exports_->cvmfs_operations.statfs(req, ino);
  atomic_dec64(&num_operations_);
}


#ifdef __APPLE__
static void stub_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
                          size_t size, uint32_t position)
#else
static void stub_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
                          size_t size)
#endif
{
  FileSystemFence();
  atomic_inc64(&num_operations_);
#ifdef __APPLE__
  cvmfs_exports_->cvmfs_operations.getxattr(req, ino, name, size, position);
#else
  cvmfs_exports_->cvmfs_operations.getxattr(req, ino, name, size);
#endif
  atomic_dec64(&num_operations_);
}


static void stub_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size) {
  FileSystemFence();
  atomic_inc64(&num_operations_);
  cvmfs_exports_->cvmfs_operations.listxattr(req, ino, size);
  atomic_dec64(&num_operations_);
}


/**
 * The callback used when fuse is parsing all the options
 * We separate CVMFS options from FUSE options here.
 *
 * \return On success zero, else non-zero
 */
static int ParseFuseOptions(void *data __attribute__((unused)), const char *arg,
                            int key, struct fuse_args *outargs)
{
  unsigned arglen = 0;
  if (arg)
    arglen = strlen(arg);
  switch (key) {
    case FUSE_OPT_KEY_OPT:
      // Check if it a cvmfs option
      if ((arglen > 0) && (arg[0] != '-')) {
        const char **o;
        for (o = (const char**)cvmfs_array_opts; *o; o++) {
          unsigned olen = strlen(*o);
          if ((arglen > olen && arg[olen] == '=') &&
              (strncasecmp(arg, *o, olen) == 0))
            return 0;
        }
      }
      return 1;

    case FUSE_OPT_KEY_NONOPT:
      // first: repository name, second: mount point
      if (!repository_name_) {
        repository_name_ = new string(arg);
      } else {
        if (mount_point_)
          return 1;
        mount_point_ = new string(arg);
      }
      return 0;

    case KEY_HELP:
      Usage(outargs->argv[0]);
      exit(0);
    case KEY_VERSION:
      LogCvmfs(kLogCvmfs, kLogStdout, "CernVM-FS version %s\n",
               PACKAGE_VERSION);
      exit(0);
    case KEY_FOREGROUND:
      foreground_ = true;
      return 0;
    case KEY_SINGLETHREAD:
      single_threaded_ = true;
      return 0;
    case KEY_DEBUG:
      fuse_opt_add_arg(outargs, "-d");
      debug_mode_ = true;
      return 0;
    default:
      LogCvmfs(kLogCvmfs, kLogStderr, "internal option parsing error");
      abort();
  }
}


static fuse_args *ParseCmdLine(int argc, char *argv[]) {
  struct fuse_args *mount_options = new fuse_args();
  CvmfsOptions cvmfs_options;
  memset(&cvmfs_options, 0, sizeof(cvmfs_options));

  mount_options->argc = argc;
  mount_options->argv = argv;
  mount_options->allocated = 0;
  if ((fuse_opt_parse(mount_options, &cvmfs_options, cvmfs_array_opts,
                      ParseFuseOptions) != 0) ||
      !mount_point_ || !repository_name_)
  {
    delete mount_options;
    return NULL;
  }
  if (cvmfs_options.config) {
    config_files_ = new string(cvmfs_options.config);
    free(cvmfs_options.config);
  }
  uid_ = cvmfs_options.uid;
  gid_ = cvmfs_options.gid;
  grab_mountpoint_ = cvmfs_options.grab_mountpoint;

  return mount_options;
}


static void SetFuseOperations(struct fuse_lowlevel_ops *loader_operations) {
  memset(loader_operations, 0, sizeof(*loader_operations));

  loader_operations->init        = stub_init;
  loader_operations->destroy     = stub_destroy;

  loader_operations->lookup      = stub_lookup;
  loader_operations->getattr     = stub_getattr;
  loader_operations->readlink    = stub_readlink;
  loader_operations->open        = stub_open;
  loader_operations->read        = stub_read;
  loader_operations->release     = stub_release;
  loader_operations->opendir     = stub_opendir;
  loader_operations->readdir     = stub_readdir;
  loader_operations->releasedir  = stub_releasedir;
  loader_operations->statfs      = stub_statfs;
  loader_operations->getxattr    = stub_getxattr;
  loader_operations->listxattr   = stub_listxattr;
}


static CvmfsExports *LoadLibrary(const bool debug_mode,
                                 LoaderExports *loader_exports)
{
  string library_name = "cvmfs_fuse";
  if (debug_mode)
    library_name += "_debug";
  library_name = platform_libname(library_name);

  library_handle_ = dlopen(library_name.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!library_handle_)
    return NULL;

  CvmfsExports **exports_ptr =
    reinterpret_cast<CvmfsExports **>(dlsym(library_handle_, "g_cvmfs_exports"));
  if (!exports_ptr)
    return NULL;

  if (loader_exports) {
    LoadEvent *load_event = new LoadEvent();
    load_event->timestamp = time(NULL);
    load_event->so_version = (*exports_ptr)->so_version;
    loader_exports->history.push_back(load_event);
  }

  return *exports_ptr;
}

}  // namespace loader


using namespace loader;

// Making OpenSSL (libcrypto) thread-safe
pthread_mutex_t *gLibcryptoLocks;

static void CallbackLibcryptoLock(int mode, int type,
                                  const char *file, int line) {
  (void)file;
  (void)line;

  int retval;

  if (mode & CRYPTO_LOCK) {
    retval = pthread_mutex_lock(&(gLibcryptoLocks[type]));
  } else {
    retval = pthread_mutex_unlock(&(gLibcryptoLocks[type]));
  }
  assert(retval == 0);
}

static unsigned long CallbackLibcryptoThreadId() {
  return platform_gettid();
}

static void SetupLibcryptoMt() {
  gLibcryptoLocks = static_cast<pthread_mutex_t *>(OPENSSL_malloc(
                                                                  CRYPTO_num_locks() * sizeof(pthread_mutex_t)));
  for (int i = 0; i < CRYPTO_num_locks(); ++i) {
    int retval = pthread_mutex_init(&(gLibcryptoLocks[i]), NULL);
    assert(retval == 0);
  }

  CRYPTO_set_id_callback(CallbackLibcryptoThreadId);
  CRYPTO_set_locking_callback(CallbackLibcryptoLock);
}

static void CleanupLibcryptoMt(void) {
  CRYPTO_set_locking_callback(NULL);
  for (int i = 0; i < CRYPTO_num_locks(); ++i)
    pthread_mutex_destroy(&(gLibcryptoLocks[i]));

  OPENSSL_free(gLibcryptoLocks);
}


int main(int argc, char *argv[]) {
  // Set a decent umask for new files (no write access to group/everyone).
  // We want to allow group write access for the talk-socket.
  umask(007);

  int retval;
  
  // Jump into alternative process flavors (e.g. shared cache manager)
  // We are here due to a fork+execve (ManagedExec in util.cc)
  if ((argc > 1) && (strstr(argv[1], "__") == argv[1])) {
    debug_mode_ = getenv("__CVMFS_DEBUG_MODE__") != NULL;
    cvmfs_exports_ = LoadLibrary(debug_mode_, NULL);
    if (!cvmfs_exports_)
      return kFailLoadLibrary;
    return cvmfs_exports_->fnAltProcessFlavor(argc, argv);
  }

  SetupLibcryptoMt();

  // Option parsing
  struct fuse_args *mount_options;
  mount_options = ParseCmdLine(argc, argv);
  if (!mount_options) {
    Usage(argv[0]);
    return kFailOptions;
  }
  options::Init();
  if (config_files_) {
    vector<string> tokens = SplitString(*config_files_, ':');
    for (unsigned i = 0, s = tokens.size(); i < s; ++i) {
      options::ParsePath(tokens[i]);
    }
  } else {
    options::ParseDefault(*repository_name_);
  }
  loader_exports_ = new LoaderExports();
  loader_exports_->loader_version = PACKAGE_VERSION;
  loader_exports_->boot_time = time(NULL);
  loader_exports_->program_name = argv[0];
  loader_exports_->foreground = foreground_;
  loader_exports_->repository_name = *repository_name_;
  loader_exports_->mount_point = *mount_point_;
  if (config_files_)
    loader_exports_->config_files = *config_files_;
  else
    loader_exports_->config_files = "";
  
  string parameter;

  // Logging
  if (options::GetValue("CVMFS_SYSLOG_LEVEL", &parameter))
    SetLogSyslogLevel(String2Uint64(parameter));
  else
    SetLogSyslogLevel(3);
  SetLogSyslogPrefix(*repository_name_);

  // Number of file descriptors
  if (options::GetValue("CVMFS_NFILES", &parameter)) {
    uint64_t nfiles = String2Uint64(parameter);
    struct rlimit rpl;
    memset(&rpl, 0, sizeof(rpl));
    getrlimit(RLIMIT_NOFILE, &rpl);
    if (rpl.rlim_max < nfiles)
      rpl.rlim_max = nfiles;
    rpl.rlim_cur = nfiles;
    retval = setrlimit(RLIMIT_NOFILE, &rpl);
    if (retval != 0) {
      PrintError("Failed to set maximum number of open files, "
                 "insufficient permissions");
      return kFailPermission;
    }
  }

  // Grab mountpoint
  if (grab_mountpoint_) {
    if ((chown(mount_point_->c_str(), uid_, gid_) != 0) ||
        (chmod(mount_point_->c_str(), 0755) != 0))
    {
      PrintError("Failed to grab mountpoint (" + StringifyInt(errno) + ")");
      return kFailPermission;
    }
  }

  // Drop credentials
  if ((uid_ != 0) || (gid_ != 0)) {
    LogCvmfs(kLogCvmfs, kLogStdout, "CernVM-FS: running with credentials %d:%d",
             uid_, gid_);
    if ((setgid(gid_) != 0) || (setuid(uid_) != 0)) {
      PrintError("Failed to drop credentials");
      return kFailPermission;
    }
  }

  if (single_threaded_) {
    LogCvmfs(kLogCvmfs, kLogStdout,
             "CernVM-FS: running in single threaded mode");
  }
  if (debug_mode_) {
    LogCvmfs(kLogCvmfs, kLogStdout,
             "CernVM-FS: running in debug mode");
  }

  // Options are not needed anymore
  options::Fini();

  // Load and initialize cvmfs library
  LogCvmfs(kLogCvmfs, kLogStdout | kLogNoLinebreak,
           "CernVM-FS: loading Fuse module... ");
  cvmfs_exports_ = LoadLibrary(debug_mode_, loader_exports_);
  if (!cvmfs_exports_) {
    LogCvmfs(kLogCvmfs, kLogStderr, "failed to load cvmfs library: %s",
             dlerror());
    return kFailLoadLibrary;
  }
  retval = cvmfs_exports_->fnInit(loader_exports_);
  if (retval != kFailOk) {
    LogCvmfs(kLogCvmfs, kLogStderr, "%s (%d)",
             cvmfs_exports_->fnGetErrorMsg().c_str(), retval);
    return retval;
  }
  LogCvmfs(kLogCvmfs, kLogStdout, "done");

  // Mount
  LogCvmfs(kLogCvmfs, kLogSyslog,
           "CernVM-FS: linking %s to repository %s",
           mount_point_->c_str(), repository_name_->c_str());
  atomic_init64(&num_operations_);
  atomic_init32(&blocking_);

  struct fuse_chan *channel;
  channel = fuse_mount(mount_point_->c_str(), mount_options);
  if (!channel) {
    PrintError("Failed to create Fuse channel");
    return kFailMount;
  }
  LogCvmfs(kLogCvmfs, kLogStdout, "CernVM-FS: mounted cvmfs on %s",
           mount_point_->c_str());

  struct fuse_lowlevel_ops loader_operations;
  SetFuseOperations(&loader_operations);
  struct fuse_session *session;
  session = fuse_lowlevel_new(mount_options, &loader_operations,
                              sizeof(loader_operations), NULL);
  if (!session) {
    PrintError("Failed to create Fuse session");
    return kFailMount;
  }

  if (!foreground_)
    Daemonize();

  cvmfs_exports_->fnSpawn();

  retval = fuse_set_signal_handlers(session);
  assert(retval == 0);
  fuse_session_add_chan(session, channel);
  if (single_threaded_)
    retval = fuse_session_loop(session);
  else
    retval = fuse_session_loop_mt(session);

  cvmfs_exports_->fnFini();

  // Unmount
  fuse_session_remove_chan(channel);
  fuse_remove_signal_handlers(session);
  fuse_session_destroy(session);
  fuse_unmount(mount_point_->c_str(), channel);
  fuse_opt_free_args(mount_options);
  channel = NULL;
  session = NULL;
  mount_options = NULL;

  dlclose(library_handle_);
  library_handle_ = NULL;

  LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslog, "CernVM-FS: unmounted %s (%s)",
           mount_point_->c_str(), repository_name_->c_str());

  CleanupLibcryptoMt();

  if (retval != 0)
    return kFailFuseLoop;
  return kFailOk;
}
