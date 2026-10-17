Write-Host "Building main.exe..." -ForegroundColor Cyan

g++ -std=c++17 -mwindows -static -static-libgcc -static-libstdc++ src/main.cpp -Isrc -lwininet -lole32 -loleaut32 -luuid -o main.exe

if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed." -ForegroundColor Red
    exit $LASTEXITCODE
}

Write-Host "Updating Gmail_App directory..." -ForegroundColor Cyan
New-Item -ItemType Directory -Force -Path "Gmail_App/ui" | Out-Null
Copy-Item "main.exe" -Destination "Gmail_App" -Force
Copy-Item "src/webview2/WebView2Loader.dll" -Destination "Gmail_App" -Force
Copy-Item ".env" -Destination "Gmail_App" -Force
Copy-Item "ui/*" -Destination "Gmail_App/ui" -Recurse -Force

Write-Host "Build and deployment completed successfully!" -ForegroundColor Green
Write-Host "Run command: Start-Process Gmail_App\main.exe" -ForegroundColor Yellow
