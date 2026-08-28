@ECHO Building TomatOS

@if "%1" == "-all" goto sec_all
@if "%1" == "-quick" goto sec_quick
@if "%1" == "-setup" goto sec_setup
@if "%1" == "-help" goto sec_help

:sec_help
@ECHO TomatOS Building Tool v2
@ECHO Use with following parameters:
@ECHO build [-all][-quick][-help]
@ECHO -all     Rebuild the whole project
@ECHO -quick   Only build with quickbuild.bat settings
@ECHO -help    Displays this message
@goto ende

:sec_all
@CALL _setenv.bat
@CALL _compileASM.bat
@CALL _compileC.bat
@CALL _link.bat
@CALL _cleanup.bat
@CALL _VFS.bat -copy .\bin\kernel.bin A:\kernel.bin
@REM @CALL _CopyFloppy.bat -copy .\bin\kernel.bin A:\kernel.bin
@ECHO finished!

@_setenv.bat
@goto ende

:sec_quick
@ECHO QUICK SECTION
@goto ende

:sec_setup
@Setup.bat
@goto ende

:ende
