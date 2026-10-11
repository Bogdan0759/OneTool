10: PRINT "hello on my guess number game!"
20: DECLARE VAR guess
30: DECLARE VAR number
40: DECLARE VAR attempts

; seed random generator
50: CALL "libc.so.6" time INT1 0
60: CALL "libc.so.6" srand INT1 OPRES

; generate random number between 1 and 100
70: CALL "libc.so.6" rand INT0
80: DIV OPRES 21474836
90: ADD OPRES 1
100: SET number OPRES
110: SET attempts 0

120: PRINT "guess the number (1-100):"
130: READINT guess

140: ADD attempts 1
150: SET attempts OPRES

160: IF guess E number
170: GOTOF 270
180: ENDIF

190: IF guess S number
200: PRINT "too small"
210: GOTOF 120
220: ENDIF

230: IF guess B number
240: PRINT "too big"
250: GOTOF 120
260: ENDIF

270: PRINT "you guessed it!"
280: PRINT "you guessed it in"
290: PRINT attempts
300: PRINT "attempts!"


