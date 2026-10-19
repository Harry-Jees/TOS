# NVIDIA NIM setup

Hawk reads the NVIDIA API credential from `.env` beside `hawk.exe` at application startup. The application does not use API keys from `hawk.ini`, process environment variables, the model settings dialog, or command-line arguments.

## Setup

1. Open the `hawk-os` folder in the repository.
2. Copy `.env.example` to a new file named `.env` in the same folder as `hawk.exe`.
3. Edit `.env` and replace the placeholder value for `NVIDIA_API_KEY` with your key from NVIDIA NIM.
4. Keep `NVIDIA_BASE_URL=https://integrate.api.nvidia.com/v1` unless your NVIDIA endpoint specifies otherwise.
5. Set `NVIDIA_MODEL` to a model ID available to your NVIDIA account.
6. Restart Hawk.

Example file format:

```dotenv
NVIDIA_API_KEY=nvapi-REPLACE_WITH_YOUR_KEY
NVIDIA_BASE_URL=https://integrate.api.nvidia.com/v1
NVIDIA_MODEL=nvidia/nemotron-3.5-super-vl-120b-a12b
```

Do not commit `.env` or share it in screenshots. Git ignores this file. If calls fail, check the NVIDIA model ID, endpoint, account access, quota, and the activity log. Hawk sends an OpenAI-compatible chat-completions request to NVIDIA NIM; the credential itself must be an NVIDIA key.

Screenshot-based planning requires a vision-capable model and explicit screenshot-upload consent.