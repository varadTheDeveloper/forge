# Manual tests

These need a person at a console, so `run-runtime-tests.ps1` cannot run them.

| Script | What to do | Expected |
|---|---|---|
| `sigint-listener.js` | Run it, press Ctrl+C once. | Prints `got SIGINT`, then `cleaned up`, exits with code 0. |
| `sigint-default.js` | Run it, press Ctrl+C once. | The busy loop stops at once; exit code 130 (`echo $LASTEXITCODE`). |
| `sigint-stuck.js` | Run it, press Ctrl+C twice. | The listener never gets to run (the loop is busy); the second Ctrl+C ends the process with code 130. |
| `unicode.js` | Run it in Windows Terminal and in the old console window. | Every line shows the same characters as the comments in the file. |
