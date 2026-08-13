/* mingw-w64 case shim. ViGEmClient.cpp includes <Windows.h> and <SetupAPI.h>
 * with capital letters; mingw-w64 ships them lowercase and Linux filesystems
 * are case sensitive, so the vendored translation unit will not compile
 * without this directory on its include path.
 *
 * #include_next (a GCC extension, and this whole path is GCC-only) resumes
 * the header search *after* the directory this file was found in. That is
 * deliberately not a symlink to /usr/x86_64-w64-mingw32/include/windows.h:
 * a symlink hard-codes one distribution's sysroot into the repository, and
 * on a case-insensitive filesystem a plain `#include <windows.h>` here would
 * re-enter this same file. #include_next is correct in both cases.
 *
 * Do not add anything else to this directory. It exists to fix two file
 * names, nothing more.
 */
#include_next <windows.h>
