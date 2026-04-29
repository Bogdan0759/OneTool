DECLARE VAR final_check
PRINT "1: Start"
GOTO 6
PRINT "3: Back from first call"
GOTO 9
PRINT "5: Back from second call. End of Main."
RETURN "STOP"
PRINT "7: Sub 1 executing"
RETURN "VAL_A"
PRINT "9: Sub 2 executing"
GOTO 7
PRINT "11: Back in Sub 2"
RETURN "VAL_B"