#!/bin/bash
# commit t290/mesa, push to fork, wait for the mesa-ios run, print its errors (Git Bash)
cd /d/test1/ios-sweet
G=/d/test1/tools-win/gh/bin/gh.exe
git add t290/mesa && git commit -q -m "${1:-t290 mesa: iOS build iteration}

Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>" && git push -q fork t290 2>&1 | tail -1
sleep 15
id=$("$G" run list -R terx1232/ios-sweet -w mesa-ios -L 1 --json databaseId -q '.[0].databaseId')
echo "run $id"
"$G" run watch "$id" -R terx1232/ios-sweet > /dev/null 2>&1
"$G" run view "$id" -R terx1232/ios-sweet 2>&1 | sed -n 5,6p
"$G" run view "$id" -R terx1232/ios-sweet --log 2>&1 > /tmp/mesa-ci.log
grep -E " ERROR:|: error: |FAILED: |ninja: build stopped|Build	[^ ]* [0-9]+$" /tmp/mesa-ci.log |
	grep -v "testfile\|mtls-dialect\|linker command failed\|unknown warning option\|wd4189\|lifetime-dse" | head -${2:-60}
