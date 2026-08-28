
@if not "%1" == "-copy" goto ende

@if "%2" == "" goto ende
@if "%3" == "" goto ende

@COPY %2 %3

@echo Copied!

:ende
