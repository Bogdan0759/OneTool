; Test 5: System, Files and Strings
PRINT "--- Test 5: System and Strings ---"

SET str1 "Hello"
SET str2 " World!"

STRCONCAT combined str1 str2
STRLEN combined len
PRINT "Combined string:"
PRINT combined
PRINT "Length:"
PRINT len

PRINT "Writing to test.txt..."
FWRITE "test.txt" combined

PRINT "Reading from test.txt..."
FREAD "test.txt" content
PRINT content

PRINT "Running ls -la test.txt..."
SYSTEM "ls -la test.txt" code
PRINT "Exit code:"
PRINT code

PRINT "Cleaning up test.txt..."
SYSTEM "rm test.txt" code
