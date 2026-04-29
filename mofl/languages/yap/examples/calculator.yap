DECLARE VAR num1
DECLARE VAR num2
DECLARE VAR op_code
PRINT "Enter first number:"
READINT num1
PRINT "Enter second number:"
READINT num2
PRINT "Op: 1-ADD, 2-SUB, 3-MUL, 4-DIV"
READINT op_code
IF op_code E 1
ADD num1 num2
PRINT OPRES
GOTO 28
ENDIF
IF op_code E 2
SUB num1 num2
PRINT OPRES
GOTO 28
ENDIF
IF op_code E 3
MUL num1 num2
PRINT OPRES
GOTO 28
ENDIF
IF op_code E 4
DIV num1 num2
PRINT OPRES 
ENDIF
PRINT "Done"
