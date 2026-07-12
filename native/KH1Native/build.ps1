$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild  = & $vswhere -latest -requires Microsoft.Component.MSBuild -find "MSBuild\**\Bin\MSBuild.exe" |
            Select-Object -First 1
& $msbuild "$PSScriptRoot\KH1Native.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /nologo
exit $LASTEXITCODE
