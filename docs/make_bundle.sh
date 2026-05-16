#!/bin/sh

echo '<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>OneTool — Documentation</title>
</head>
<body>' > index.html

for f in overview.html \
         libs.html \
         srapi.html \
         tools-filesystem.html \
         tools-network.html \
         tools-system.html \
         tools-dev.html \
         tools-power.html \
         tui.html \
         mofl.html \
         config-extend.html
do
    sed -n '/<body>/,/<\/body>/p' "$f" \
        | sed '1s/.*<body>//; $s/<\/body>.*//'
done >> index.html

echo '</body>
</html>' >> index.html