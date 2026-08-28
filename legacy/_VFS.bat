
@if not "%1" == "-copy" goto ende

@if "%2" == "" goto ende
@if "%3" == "" goto ende


@REM @G:\tools\vfd\vfd open .\bin\dev_kernel_grub.img
@G:\tools\vfd\vfd open G:\bin\dev_kernel_grub.img
@COPY %2 %3
@G:\tools\vfd\vfd Close

@echo Copied!

:ende
