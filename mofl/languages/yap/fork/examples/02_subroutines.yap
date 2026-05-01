; Test 2: Subroutines and Labels
PRINT "--- Test 2: Subroutines ---"

PRINT "Calling sub..."
GOSUB print_hello
PRINT "Back from sub, return value:"
PRINT RETVAL

JUMP skip
PRINT "This should be skipped!"

LABEL skip
PRINT "Skipped successfully!"
EXIT 0

LABEL print_hello
PRINT "Hello from subroutine!"
RETURN "sub_success"
