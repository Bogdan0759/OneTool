; Test 1: Loops and Math with destinations
PRINT "--- Test 1: Loops and Math ---"

SET count 0
SET sum 0

WHILE count S 10
  ADD count count 1
  ADD sum sum count
  PRINT "Count is:"
  PRINT count
ENDWHILE

PRINT "Final sum 1..10 is:"
PRINT sum

MOD rem 10 3
PRINT "10 mod 3 is:"
PRINT rem
