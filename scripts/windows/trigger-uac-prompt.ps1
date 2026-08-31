$ErrorActionPreference = "Stop"

# Run this script in a non-elevated interactive session. The elevated child is
# intentionally short-lived; the UAC desktop transition is the behavior under test.
Start-Process powershell.exe -Verb RunAs -ArgumentList @(
    "-NoProfile",
    "-Command",
    "Start-Sleep -Seconds 5"
)
