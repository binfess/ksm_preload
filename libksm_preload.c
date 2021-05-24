/* By Brice Arnould <unbrice@vleu.net>
 * Copyright (C) 2011 Gandi SAS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

/* Enables KSM on heap-allocated memory.
 * Usage: make
 *        LD_PRELOAD+=./ksm-preload.so command args ...
 */


#define _GNU_SOURCE             // dlsym(), mremap()
#include <dlfcn.h>              // dlsym()
#include <sys/mman.h>           // mmap(), mmap2(), mremap()
#include <unistd.h>             // syscall()
#include <sys/syscall.h>        // SYS_mmap, SYS_mmap2

#include <assert.h>
#include <error.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>              // fprintf(), stderr
#include <stdint.h>             // uintptr_t
#include <stdlib.h>

/* The default value for merge_threshold */
static const char *const MERGE_THRESHOLD_ENV_NAME = "KSMP_MERGE_THRESHOLD";

#ifdef GCC
# define likely(x)      __builtin_expect((x),1)
#else
# define likely(x)      (x)
#endif

/******** GLOBAL STATE ********/

/* Aliases for function types.
 * Just a little spoon of syntactic sugar to help the medicine go down
 */
typedef void *calloc_function (size_t nmemb, size_t size);
typedef void *malloc_function (size_t size);
typedef void *mmap_function (void *start, size_t length, int prot, int flags,
                             int fd, off_t offset);
typedef void *mremap_function (void *old_address, size_t old_length,
                               size_t new_length, int flags, ...);
typedef void *realloc_function (void *addr, size_t size);

/* Declares the libc version of the functions we hook */
extern calloc_function __libc_calloc;
extern malloc_function __libc_malloc;
extern mmap_function __mmap;
extern realloc_function __libc_realloc;

/* This structure contains all global variables. */
static struct
{
  /* The functions that the program would be using if we weren't preloaded.
   * Temporarily set to "safe" values during initialisation
   */
  calloc_function *ext_calloc;
  malloc_function *ext_malloc;
  mmap_function *ext_mmap;
  mremap_function *ext_mremap;
  realloc_function *ext_realloc;
  /* The page size, this value is temporary and will be fixed
   * by setup()
   */
  unsigned long page_size;
  /* Zones smaller than this won't be merged */
  int merge_threshold;
} globals =
{
#if __GLIBC_PREREQ(2,11) || KSMP_FORCE_LIBC
  __libc_calloc,                // libc's calloc
  __libc_malloc,		// libc's malloc
  __mmap,			// libc's mmap
  NULL,				// mremap, unused during initialisation
  __libc_realloc,		// libc's realloc
  4096,				// page_size
  4096 * 8			// merge threshold
#else
#error This version of ksm_preload has not been tested with your	\
  libC. Please define KSMP_FORCE_LIBC to 1 (-DKSMP_FORCE_LIBC=1) and	\
  tell me about the result.
#endif
};

/******** SETUP ********/

#ifdef DEBUG
#define debug_printf(fmt, ...) fprintf(stderr, \
				       "ksm_preload: " fmt "\n" __VA_OPT__(,) __VA_ARGS__)
#define debug_puts(str) debug_printf(str)
#else
#define debug_printf(fmt, ...)
#define debug_puts(str)
#endif

/* Gets an environment variable from its name and parses it as a
 * positive integer.
 * Returns the parsed value truncated to INT_MAX, -1 if undefined or invalid.
 */
static int
get_int_from_environment (const char *var_name)
{
  char *var_string = getenv (var_name);
  char *var_string_end = var_string;
  long int var_value;		// var_string as a long itn

  if (NULL == var_string)
    return -1;
  else
    var_value = strtol (var_string, &var_string_end, 10);

  /* Validates strtol's return value */
  if (*var_string_end != '\0' || var_value < 0)
    {
      debug_printf ("Invalid environment variable %s=%s, a"
		    " positive integer was expected.", var_name, var_string);
      return -1;
    }
  else if (var_value > INT_MAX)
    {
      debug_printf ("Truncated %s to INT_MAX(%i) ", var_name, INT_MAX);
      return INT_MAX;
    }
  else
    return (int) var_value;
}

/* Just like dlsym but error()s in case of failure */
static void *
xdlsym (void *handle, const char *symbol)
{
  void *res = dlsym (handle, symbol);
  if (res)
    return res;
  else
    {
      error (1, 0, "failed to load %s : %s", symbol, dlerror ());
      return NULL;
    }
}

/* Sets the globals.* variables */
static void
setup ()
{
  int env_merge_treshold;
  /* Loads the symbols from the next library using the libc functions
   * We will set them at once to avoid a situation where we would be
   * using some of them, and some of the default ones
   */
  calloc_function *dl_calloc = xdlsym (RTLD_NEXT, "calloc");
  malloc_function *dl_malloc = xdlsym (RTLD_NEXT, "malloc");
  mmap_function *dl_mmap = xdlsym (RTLD_NEXT, "mmap");
  mremap_function *dl_mremap = xdlsym (RTLD_NEXT, "mremap");
  realloc_function *dl_realloc = xdlsym (RTLD_NEXT, "realloc");

  /* Get parameters from the environment */
  globals.page_size = (long unsigned) sysconf (_SC_PAGESIZE);
  env_merge_treshold = get_int_from_environment (MERGE_THRESHOLD_ENV_NAME);
  if (env_merge_treshold >= 0)
    globals.merge_threshold = env_merge_treshold;

  /* Activates the symbols from the next library */
  globals.ext_calloc = dl_calloc;
  globals.ext_malloc = dl_malloc;
  globals.ext_mmap = dl_mmap;
  globals.ext_mremap = dl_mremap;
  globals.ext_realloc = dl_realloc;

  debug_puts ("Setup done.");
}

/******** UTILITIES FOR WRAPPERS ********/

static void
lazily_setup ()
{
  /* Allows to be sure that only one thread is calling setup() */
  static pthread_mutex_t mutex = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;
  /* True if setup() has been called and returned */
  static bool setup_done = false;

  /* Quickly returns if the job was already done */
  __sync_synchronize ();	// updates globals.* variables
  if (likely (setup_done))
    return;

  // <mutex>
  if (pthread_mutex_lock (&mutex) == EDEADLK)
    return;			// Recursive call

  if (!setup_done)		// Might have been called since last check
    {
      setup ();
      setup_done = true;
    }

  // </mutex>
  pthread_mutex_unlock (&mutex);
}

/* Issues a madvise(..., MADV_MERGEABLE) if len is big enough and flags are rights.
 * Flags are ignores if flags == -1
 */
static void
merge_if_profitable (void *address, size_t length, int flags)
{

  /* Rounds address to its page */
  const uintptr_t raw_address = (uintptr_t) address;
  const uintptr_t page_address =
    (raw_address / globals.page_size) * globals.page_size;
  assert (page_address <= raw_address);

  /* Computes the new length */
  const size_t new_length = length + (size_t) (raw_address - page_address);

  if (new_length <= globals.merge_threshold || NULL == address)
    return;
  /* Checks that required flags are present and that forbidden ones are not */
  else if (flags == -1		// flags are unknown
	   // Checks for required flags, avoids the stacks
	   || ((flags & MAP_PRIVATE) && (flags & MAP_ANONYMOUS)
	       && !(flags & MAP_GROWSDOWN) && !(flags & MAP_STACK)))
    {
      if (0 != madvise ((void *) page_address, new_length, MADV_MERGEABLE))
	debug_puts ("madvise() failed");
      else
	debug_printf ("Sharing %zu bytes from %p", new_length, page_address);
    }
  else
    debug_puts ("Not sharing (flags filtered)");
}

/******** WRAPPERS ********/

/* Just like calloc() but calls merge_if_profitable */
void *
calloc (size_t nmemb, size_t size)
{
  lazily_setup ();
  void *res = globals.ext_calloc (nmemb, size);
  debug_printf ("calloc (%zu, %zu) = %p", nmemb, size, res);
  merge_if_profitable (res, size, -1);
  return res;
}

/* Just like malloc() but calls merge_if_profitable */
void *
malloc (size_t size)
{
  lazily_setup ();
  void *res = globals.ext_malloc (size);
  debug_printf ("malloc (%zu) = %p", size, res);
  merge_if_profitable (res, size, -1);
  return res;
}

/* Just like mmap() but calls merge_if_profitable */
void *
mmap (void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
  lazily_setup ();
  void *res = globals.ext_mmap (addr, length, prot, flags, fd, offset);
  debug_printf ("mmap (%p, %zu, %d, %d, %d, %llu) = %p",
		addr, length, prot, flags, fd, (unsigned long long)offset, res);
  merge_if_profitable (res, length, flags);
  return res;
}

/* Just like mremap() but calls merge_if_profitable */
void *
mremap (void *old_address, size_t old_length, size_t new_length, int flags,
	...)
{
  lazily_setup ();
  void *res;
  if (flags & MREMAP_FIXED)
    {
      /* This is the five-arguments version of mremap. */
      // It sometimes happens that the kernel's API is so ugly…
      void *target_address;
      va_list extra_args;
      va_start (extra_args, flags);
      target_address = va_arg (extra_args, void *);
      va_end (extra_args);
      res = globals.ext_mremap (old_address, old_length, new_length, flags,
				target_address);
    }
  else
    res = globals.ext_mremap (old_address, old_length, new_length, flags);
  debug_printf ("mremap (%p, %zu, %zu, %d, ...) = %p",
		old_address, old_length, new_length, flags, res);
  merge_if_profitable (res, new_length, -1);
  return res;
}

/* Just like realloc() but calls merge_if_profitable */
void *
realloc (void *addr, size_t size)
{
  lazily_setup ();
  void *res = globals.ext_realloc (addr, size);
  debug_printf ("realloc (%p, %zu) = %p", addr, size, res);
  merge_if_profitable (res, size, -1);
  return res;
}
