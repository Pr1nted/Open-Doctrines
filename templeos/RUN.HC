/* Start Open Doctrines from the compiled binary.
 *
 *     #include "RUN"     -- loads it; then the two lines below, by hand
 *
 * ── WHY THIS IS NOT ONE COMMAND ──
 *
 * A file is compiled in full before any of it runs, so a Load() on line one
 * still leaves ODStart unknown to line two. #exe runs at compile time and
 * gets past that -- but the symbol arrives as an ADDRESS, not as a function:
 * `ODStart;` at the shell yields a pointer and `ODStart();` is "Invalid
 * class", so it has to be called through a declared pointer. Declaring that
 * pointer in the same file that loads the binary faults, and the shell can do
 * it only because each command line is compiled AND run before the next one
 * is read.
 *
 * So: this loads, and the caller enters. Two more lines, typed:
 *
 *     U0 (*f)(U8 *w) = ODStart;
 *     (*f)("world.odw");
 *
 * That is the whole gap between here and a one-word launch, and it is
 * written down rather than papered over.
 */
#exe {Load("ODBIN.BIN");};

"Open Doctrines loaded. Now:\n";
"  U0 (*f)(U8 *w) = ODStart;\n";
"  (*f)(\"world.odw\");\n";
