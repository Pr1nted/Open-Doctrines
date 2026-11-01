# DJGPP has no threads, and the game's DOS build takes none (OD_NO_THREADS).
#
# The test targets further down CMakeLists.txt ask for Threads with REQUIRED,
# unconditionally; the stock FindThreads looks for pthread.h, finds nothing on
# DJGPP and stops the configure. They are never built for DOS (each is
# EXCLUDE_FROM_ALL and the DOS build asks for OpenDoctrines alone), so this
# answers the question with an empty target. Only on the module path when
# cross-compiling with DJGPP; see OD_DOS in CMakeLists.txt.
set(Threads_FOUND TRUE)
set(CMAKE_THREAD_LIBS_INIT "")
set(CMAKE_USE_PTHREADS_INIT FALSE)
if(NOT TARGET Threads::Threads)
    add_library(Threads::Threads INTERFACE IMPORTED)
endif()
