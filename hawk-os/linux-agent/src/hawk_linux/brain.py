"""Hugging Face model setup and local inference for the HAWK brain."""
from __future__ import annotations
from pathlib import Path

DEFAULT_MODEL = "Qwen/Qwen3.5-4B"

def download_model(model_id: str = DEFAULT_MODEL, local_dir: str | None = None) -> str:
    """Download model artifacts from Hugging Face Hub; explicit user action."""
    try:
        from huggingface_hub import snapshot_download
    except ImportError as exc:
        raise RuntimeError("Install model extras first: pip install -e '.[model]'") from exc
    target = Path(local_dir).expanduser() if local_dir else None
    return snapshot_download(repo_id=model_id, local_dir=str(target) if target else None)

class LocalBrain:
    """Local inference. Model output is advisory; never execute it as shell code."""
    def __init__(self, model_id: str = DEFAULT_MODEL):
        try:
            import torch
            from transformers import AutoProcessor, AutoModelForImageTextToText
        except ImportError as exc:
            raise RuntimeError("Install model extras first: pip install -e '.[model]'") from exc
        self.torch = torch
        self.processor = AutoProcessor.from_pretrained(model_id, trust_remote_code=False)
        self.model = AutoModelForImageTextToText.from_pretrained(
            model_id, device_map="auto", torch_dtype="auto", trust_remote_code=False
        )

    def ask(self, prompt: str):
        messages = [
            {"role": "system", "content": [{"type": "text", "text":
             "You are HAWK, a careful Linux sandbox research assistant. Propose plans; do not claim actions were executed. Never request secrets or attempt unauthorized sandbox escape."}]},
            {"role": "user", "content": [{"type": "text", "text": prompt}]}
        ]
        inputs = self.processor.apply_chat_template(
            messages, add_generation_prompt=True, tokenize=True,
            return_dict=True, return_tensors="pt"
        ).to(self.model.device)
        with self.torch.inference_mode():
            outputs = self.model.generate(**inputs, max_new_tokens=512, do_sample=False)
        return self.processor.decode(outputs[0][inputs["input_ids"].shape[-1]:], skip_special_tokens=True)
