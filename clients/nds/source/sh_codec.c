/* Wrapper TU. See the SOURCEDIRS note in ../Makefile for why the shared
 * sources are reached this way instead of being listed in SOURCES_C:
 *
 *   the SDK makefile derives every object path as $(BUILDDIR)/<source path>,
 *   so a source listed as ../../core/src/codec.c produces its .o INSIDE
 *   core/src/ -- outside this project, uncleanable, and shared with whatever
 *   other platform builds next. A wrapper that lives in source/ keeps the
 *   object in build/ and still compiles the one real copy of the file.
 *
 * Nothing is copied here and nothing is added. A quoted #include resolves
 * relative to the directory of the file containing the directive, so the
 * shared file's own includes still resolve from where it lives.
 */
#include "../../../core/src/codec.c"
