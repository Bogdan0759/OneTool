; Test 3: Arrays
PRINT "--- Test 3: Arrays ---"

; Set some values in an array
ARRSET my_list 0 "Apple"
ARRSET my_list 1 "Banana"
ARRSET my_list 2 "Cherry"

; Retrieve them
SET i 0
WHILE i S 3
  ARRGET item my_list i
  STRCONCAT msg "Item " i " is " item
  PRINT msg
  ADD i i 1
ENDWHILE
