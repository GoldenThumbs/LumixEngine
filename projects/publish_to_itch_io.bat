REM init developer command prompt
call "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\Common7\Tools\VsDevCmd.bat"

set msbuild_cmd=msbuild.exe
set devenv_cmd=devenv.exe
where /q devenv.exe
if not %errorlevel%==0 set devenv_cmd="C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\Common7\IDE\devenv.exe"
where /q msbuild.exe
if not %errorlevel%==0 set msbuild_cmd="C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe"

REM put butler in path or in ..\butler\
SET PATH=%PATH%;..\butler\

REM clean everything
git.exe clean -f -x -d

REM download physx
mkdir 3rdparty
cd 3rdparty
git.exe clone --depth=1 https://github.com/nem0/PhysX.git physx

REM build static physx
cd PhysX\physx
call generate_projects.bat lumix_vc16win64_static
%msbuild_cmd% "compiler\vc16win64\PhysXSDK.sln" /p:Configuration=Release /p:Platform=x64

REM deploy physx
cd ..\..\..\
if not exist "..\external\physx\lib\vs2017\win64\release_static\" mkdir ..\external\physx\lib\vs2017\win64\release_static\
del /Q ..\external\physx\lib\vs2017\win64\release_static\*
copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\PhysXCharacterKinematic_static_64.lib ..\external\physx\lib\vs2017\win64\release_static\
copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\PhysXCommon_static_64.lib			   ..\external\physx\lib\vs2017\win64\release_static\
copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\PhysXCooking_static_64.lib			   ..\external\physx\lib\vs2017\win64\release_static\
copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\PhysXExtensions_static_64.lib		   ..\external\physx\lib\vs2017\win64\release_static\
copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\PhysXFoundation_static_64.lib		   ..\external\physx\lib\vs2017\win64\release_static\
copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\PhysXPvdSDK_static_64.lib			   ..\external\physx\lib\vs2017\win64\release_static\
copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\PhysXVehicle_static_64.lib			   ..\external\physx\lib\vs2017\win64\release_static\
copy 3rdparty\PhysX\physx\bin\win.x86_64.vc141.md\release\PhysX_static_64.lib				   ..\external\physx\lib\vs2017\win64\release_static\

REM create engine project
genie.exe --embed-resources --static-physx vs2019

REM build engine.exe with bundled data
cd ..\data
tar -cvf data.tar .
move data.tar ../src/studio
cd ..\projects\
%msbuild_cmd% tmp/vs2019/LumixEngine.sln /p:Configuration=RelWithDebInfo
del ..\src\studio\data.tar

REM push gl version
mkdir itch_io
copy tmp\vs2019\bin\RelWithDebInfo\studio.exe itch_io\
butler.exe push itch_io mikulasflorek/lumix-engine:win-64-gl

REM download dx
pushd ..\plugins
git.exe clone https://github.com/nem0/lumixengine_dx.git dx
popd

REM build engine.exe with bundled data
cd ..\data
tar -cvf data.tar .
move data.tar ../src/studio
cd ..\projects\
%msbuild_cmd% tmp/vs2019/LumixEngine.sln /p:Configuration=RelWithDebInfo
del ..\src\studio\data.tar

REM push gl version
mkdir itch_io
copy tmp\vs2019\bin\RelWithDebInfo\studio.exe itch_io\
butler.exe push itch_io mikulasflorek/lumix-engine:win-64-dx
pause