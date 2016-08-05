# include <SystemConfiguration/SystemConfiguration.h>
#include <unistd.h>
#include <pwd.h>
#include <signal.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/wait.h>

// MacOS/X Code taken from http://developer.apple.com/technotes/tn/tn1145.html
// VIA: https://public.msli.com/lcs/jaf/osx_ip_change_notify.cpp

static OSStatus MoreSCErrorBoolean(Boolean success)
{
   OSStatus err = noErr;
   if (!success)
   {
      int scErr = SCError();
      if (scErr == kSCStatusOK) scErr = kSCStatusFailed;
      err = scErr;
   }
   return err;
}

static OSStatus MoreSCError(const void *value) {return MoreSCErrorBoolean(value != NULL);}
static OSStatus CFQError(CFTypeRef cf) {return (cf == NULL) ? -1 : noErr;}
static void CFQRelease(CFTypeRef cf) {if (cf != NULL) CFRelease(cf);}

// Create a SCF dynamic store reference and a corresponding CFRunLoop source.  If you add the
// run loop source to your run loop then the supplied callback function will be called when local IP
// address list changes.
static OSStatus CreateIPAddressListChangeCallbackSCF(SCDynamicStoreCallBack callback, void *contextPtr, SCDynamicStoreRef *storeRef, CFRunLoopSourceRef *sourceRef)
{
   OSStatus                err;
   SCDynamicStoreContext   context = {0, NULL, NULL, NULL, NULL};
   SCDynamicStoreRef       ref = NULL;
   CFStringRef             patterns[2] = {NULL, NULL};
   CFArrayRef              patternList = NULL;
   CFRunLoopSourceRef      rls = NULL;

   assert(callback   != NULL);
   assert( storeRef  != NULL);
   assert(*storeRef  == NULL);
   assert( sourceRef != NULL);
   assert(*sourceRef == NULL);

   // Create a connection to the dynamic store, then create
   // a search pattern that finds all entities.
   context.info = contextPtr;
   ref = SCDynamicStoreCreate(NULL, CFSTR("AddIPAddressListChangeCallbackSCF"), callback, &context);
   err = MoreSCError(ref);
   if (err == noErr)
   {
      // This pattern is "State:/Network/Service/[^/]+/IPv4".
      patterns[0] = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL, kSCDynamicStoreDomainState, kSCCompAnyRegex, kSCEntNetIPv4);
      err = MoreSCError(patterns[0]);
      if (err == noErr)
      {
         // This pattern is "State:/Network/Service/[^/]+/IPv6".
         patterns[1] = SCDynamicStoreKeyCreateNetworkServiceEntity(NULL, kSCDynamicStoreDomainState, kSCCompAnyRegex, kSCEntNetIPv6);
         err = MoreSCError(patterns[1]);
      }
   }

   // Create a pattern list containing just one pattern,
   // then tell SCF that we want to watch changes in keys
   // that match that pattern list, then create our run loop
   // source.
   if (err == noErr)
   {
       patternList = CFArrayCreate(NULL, (const void **) patterns, 2, &kCFTypeArrayCallBacks);
       err = CFQError(patternList);
   }
   if (err == noErr) err = MoreSCErrorBoolean(SCDynamicStoreSetNotificationKeys(ref, NULL, patternList));
   if (err == noErr)
   {
       rls = SCDynamicStoreCreateRunLoopSource(NULL, ref, 0);
       err = MoreSCError(rls);
   }

   // Clean up.
   CFQRelease(patterns[0]);
   CFQRelease(patterns[1]);
   CFQRelease(patternList);
   if (err != noErr)
   {
      CFQRelease(ref);
      ref = NULL;
   }
   *storeRef = ref;
   *sourceRef = rls;

   assert( (err == noErr) == (*storeRef  != NULL) );
   assert( (err == noErr) == (*sourceRef != NULL) );

   return err;
}

struct callbackstate {
  char *path;
  char *fname;
  pid_t last_pid;
  int run_again;
};

bool g_keep_running = true;
bool g_verbose = true;
#define LOG(...) do { if(g_verbose) fprintf(stderr, __VA_ARGS__); } while (0)

static void handler(struct callbackstate *state){
  if(state->last_pid){
    int status;
    if(waitpid(state->last_pid, &status, WNOHANG) > 0){
      if(WIFEXITED(status)){
        LOG("Child exited with status = %d\n",WEXITSTATUS(status));
      }
    }
    if(kill(state->last_pid, 0) != 0){
      state->run_again = 0;
    } else {
      LOG("Old process still running pid = %d\n",state->last_pid);
      state->run_again++;
      return;
    }
  }
  if(0 == access(state->path, X_OK)){
    LOG("Attempting to execute '%s'\n",state->path);
    pid_t pid = fork();
    if(pid == -1){
      LOG("Failed to fork\n");
      return;
    }
    if(pid == 0){
      execl(state->path, state->fname , "IP_CHANGED", NULL);
      perror("Failed to execute ip change handler");
      exit(EXIT_FAILURE);
    } else {
      state->last_pid = pid;
    }
  } else {
    LOG("No executable file at '%s'\n",state->path);
  }
}
static void IPConfigChangedCallback(SCDynamicStoreRef /*store*/, CFArrayRef /*changedKeys*/, void *state)
{
  LOG("IP Configuration changed,\n");
  handler((struct callbackstate *)state);
}

void sighandler(int sig){
  switch(sig){
  case SIGTERM:
  case SIGINT:
    LOG("Received signal to exit\n");
    g_keep_running = false;
    CFRunLoopStop(CFRunLoopGetCurrent());
  }
}

bool g_started = false;

void done(){
  if(g_started)
    LOG("Done listening for IP configuration changes\n");
}


#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
void usage()
{
  printf("Usage: netwatcher [-qd] [-f UTILITY ]\n" \
         " -q: quiet mode\n" \
         " -d: debug mode, stay in foreground\n" \
         "Executes UTILITY or ~/.netwatch whenever the IP " \
         "addresses of this computer change.\n");
}

int main(int argc, char **argv)
{

  close(STDIN_FILENO);
  int ch;
  bool daemonize = true;
  const char *default_fname = ".netwatch";
  const char *fname = default_fname;

  while ((ch = getopt(argc, argv, "qdh?f:")) != -1) {
    switch (ch) {
    case 'q':
      g_verbose = false;
      break;
    case 'd':
      daemonize = false;
      g_verbose = true;
      break;
    case 'f':
      fname = optarg;
      break;
    case 'h':
    case '?':
    default:
      usage();
      return EXIT_FAILURE;
    }
  }
  argc -= optind;
  argv += optind;

  if(daemonize){
     int did_daemon;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
     did_daemon = 0 == daemon(1,1); //TODO: Switch to using launchd
#pragma clang diagnostic pop

     if(!did_daemon){
       perror("Unable to daemonize");
       return EXIT_FAILURE;
     }
     LOG("Daemonized. PID = %d\n", getpid());
   } else {
     LOG("Foreground mode\n");
   }


   atexit(done);
   signal(SIGCHLD, SIG_IGN);
   signal(SIGTERM, sighandler);
   signal(SIGINT, sighandler);

   struct callbackstate state;
   state.run_again = 0;
   state.last_pid = 0;


   const char *base;
   if(fname == default_fname) {
     if ((base = getenv("HOME")) == NULL) {
       base = getpwuid(getuid())->pw_dir;
     }

     LOG("Changing to %s\n", base);
     if(-1 == chdir(base)){
       perror("Unable to chdir to home directory");
       return EXIT_FAILURE;
     }
   } else {
     if(fname[0] == '/'){
       base = "";
     } else {
       // relative
       base = getcwd(NULL, PATH_MAX);
     }
   }

   size_t baselen = strnlen(base, PATH_MAX);
   size_t flen = strnlen(fname, PATH_MAX);
   if(baselen >= PATH_MAX - flen - 1) {
     fprintf(stderr,"Path too long\n");
     return EXIT_FAILURE;
   }

   bool fslash = fname[0]=='/';
   state.path = (char *) malloc(baselen + flen + (fslash?1:2));
   if(NULL == state.path){
     perror("Unable to allocate space to hold path to utility");
     return EXIT_FAILURE;
   }
   char *at = state.path;
   memcpy(at, base, baselen);
   at += baselen;
   if(!fslash) {
     *(at++)='/';
   }
   memcpy(at, fname, flen);
   at+=flen;
   *at='\0';
   state.fname = strrchr(state.path, '/');
   if(state.fname == NULL){
     fprintf(stderr, "Unable to construct path to utility\n");
     return EXIT_FAILURE;
   } else {
     state.fname++;
   }

   if(state.fname[0] == '\0' ||
      (state.fname[0] == '.' &&
       (state.fname[1] == '\0' ||
        (state.fname[1] == '.' && state.fname[2] == '\0')))){
     fprintf(stderr, "Invalid utility path %s\n", state.path);
     return EXIT_FAILURE;
   }

   LOG("Preparing to use '%s' as utility path\n", state.path);
   if(0 != access(state.path, X_OK)){
     fprintf(stderr, "WARNING: No executable program at %s\n", state.path);
   }

   SCDynamicStoreRef storeRef = NULL;
   CFRunLoopSourceRef sourceRef = NULL;
   if (CreateIPAddressListChangeCallbackSCF(IPConfigChangedCallback, &state, &storeRef, &sourceRef) == noErr)
   {
      CFRunLoopAddSource(CFRunLoopGetCurrent(), sourceRef, kCFRunLoopDefaultMode);
      LOG("Listening for IP configuration changes...\n");
      g_started = true;
      while(g_keep_running){
        CFRunLoopRun();
        if(g_keep_running){
          if(state.last_pid != 0){
            if(state.run_again > 12){
              kill(state.last_pid, SIGKILL);
              LOG("Abandoning stubborn child pid=%d\n", state.last_pid);
              state.run_again = 0;
              state.last_pid = 0;
            } else if(state.run_again > 4){
              kill(state.last_pid, SIGHUP);
            } else if(state.run_again > 8){
            kill(state.last_pid, SIGTERM);
            }
          }
          if(state.run_again > 0){
            handler(&state);
          }
        }
      }
      CFRunLoopRemoveSource(CFRunLoopGetCurrent(), sourceRef, kCFRunLoopDefaultMode);
      CFRelease(storeRef);
      CFRelease(sourceRef);
   }
}
