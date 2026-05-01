; Test 4: Floats
PRINT "--- Test 4: Floats ---"

SET pi 3.14159
SET r 5.0

; Area = pi * r * r
MUL area pi r
MUL area area r
PRINT "Area of circle with radius 5.0 is:"
PRINT area

IF area B 50.0
  PRINT "Area is greater than 50!"
ENDIF
