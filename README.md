# SilenceRemover

As the name implies, this program removes silence from the beginning of a WAV or FLAC file. I wrote this tool since I had tons of batch-recorded samples from synthesizers which had a fixed amount of silence at the beginning.

I wrote this tool in about an hour or two, so don't expect brilliant code. Getting the job done was more important than beautiful code.

# Usage

`SilenceRemove input_file delay [forced sample rate]`
- `input_file`: Filename of one FLAC or WAV file to trim
- `delay`: Duration of silence to remove, in milliseconds (may be fractional)
- `forced sample rate`: Optional. Enforced a given sample rate for the delay to samples conversion.

# This tool works with...
- Little-Endian processors
- Visual Studio 2010
- Windows (widechar APIs are used)
- PCM and Float WAV samples, as long as they don't use WAVEFORMATEXTENSIBLE.
- FLAC samples
- Loop points in WAV format, and FLAC with embedded WAV loop points

# Batch scripting
It's advised to use this tool together with a batch file to run it over an entire directory of files.
Example:

```
REM parameters: directory, delay
cd "%1"
for /R %%F in (*.wav, *.flac) do (
"SilenceRemover.exe" "%%F" %2
if errorlevel 1 (
echo FAIL
) else (
echo %%F
)
)
pause
```

# Contact
If you have any questions, mail me through my website at http://sagamusix.de/
