DECLARE VAR counter
SET counter 1

; Строка 4: Начало цикла
PRINT counter

IF counter E 10
GOTO 11
ENDIF

ADD counter 1
SET counter OPRES

GOTO 4

; Строка 11: Финиш
PRINT "Loop Finished!"