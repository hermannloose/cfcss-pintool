/**
 * @author Hermann Loose <hermannloose@gmail.com>
 *
 * TODO(hermannloose): Add description.
 */

#include "pin.H"

#include <iostream>
INT32 Usage() {
  cerr << KNOB_BASE::StringKnobSummary() << endl;

  return -1;
}

BOOL HandleSignal(
    THREADID tid,
    INT32 signal,
    CONTEXT *context,
    BOOL hasHandler,
    const EXCEPTION_INFO *exceptionInfo,
    VOID *v) {

  switch (signal) {
    case 4:
      cerr << "CFCSS detected a control-flow violation! (SIGILL)" << endl;
      break;
    default:
      cerr << "Signal: " << signal << endl;
  }

  cerr << "EIP:    " << showbase << hex << PIN_GetContextReg(context, REG_EIP)
      << noshowbase << dec << endl;

  // Pass the signal on to the application.
  return TRUE;
}


int main(int argc, char *argv[]) {
  // Initialize PIN library. Print help message if -h(elp) is specified
  // in the command line or the command line is invalid
  if (PIN_Init(argc, argv)) {
    return Usage();
  }

  PIN_InitSymbols();

  // Intercept SIGILL, raised by UD2 instruction in CFCSS, signalling
  // a signature mismatch.
  PIN_InterceptSignal(4, HandleSignal, 0);

  // Intercept SIGSEGV, since these will count as "detected" errors as well.
  PIN_InterceptSignal(11, HandleSignal, 0);

  // Start the program, never returns.
  PIN_StartProgram();

  return 0;
}
