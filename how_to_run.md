if (!(Test-Path build)) { New-Item -ItemType Directory -Path build | Out-Null }
g++ -std=c++17 src/\*.cpp -o build/q.exe
.\build\q.exe .\examples\108q3_palindrome.q
