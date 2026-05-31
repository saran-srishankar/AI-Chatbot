import subprocess
import asyncio
import httpx
import json
import os
import struct
import pyttsx3
import tempfile
import wave
import audioop
import sys

NIOS2_TERMINAL = r"C:/intelFPGA/QUARTUS_Lite_V23.1/quartus/bin64/nios2-terminal.exe"
# include API key before running
OPENROUTER_API_KEY = ""
MODEL = "google/gemma-3-4b-it:free"

MSG_TEXT  = b'\x01'
MSG_AUDIO = b'\x02'

async def stream_llm_query(prompt):
    url = "https://openrouter.ai/api/v1/chat/completions"
    headers = {
        "Authorization": f"Bearer {OPENROUTER_API_KEY}",
        "Content-Type": "application/json"
    }
    payload = {
        "model": MODEL,
        "max_tokens": 300,
        "messages": [{"role": "user", "content": prompt}],
        "stream": True
    }
    async with httpx.AsyncClient(timeout=30) as client:
        async with client.stream("POST", url, headers=headers, json=payload) as resp:
            async for line in resp.aiter_lines():
                if line.startswith("data: "):
                    data_str = line[6:]
                    if data_str.strip() == "[DONE]":
                        break
                    try:
                        data = json.loads(data_str)
                        delta = data["choices"][0]["delta"].get("content", "")
                        if delta:
                            yield delta
                    except (json.JSONDecodeError, KeyError):
                        continue

def send_text(proc, text):
    encoded = text.encode("ascii", errors="ignore")
    if not encoded:
        return
    proc.stdin.write(MSG_TEXT)
    proc.stdin.write(encoded)
    proc.stdin.write(b"\n")
    proc.stdin.flush()

def send_text_end(proc):
    """Send empty marker so C side knows stream is done"""
    proc.stdin.write(MSG_TEXT)
    proc.stdin.write(b"\x00\n")  # null byte + newline as end marker
    proc.stdin.flush()

def send_audio(proc, pcm_data):
    num_samples = len(pcm_data)  # 1 byte per sample
    proc.stdin.write(MSG_AUDIO)
    proc.stdin.write(struct.pack("<I", num_samples))
    proc.stdin.write(pcm_data)
    proc.stdin.flush()

async def tts_to_pcm(text):
    text = text.strip()
    if not text:
        return b""

    tmp_wav = tempfile.mktemp(suffix=".wav")
    engine = pyttsx3.init()
    engine.setProperty('rate', 150)
    engine.save_to_file(text, tmp_wav)
    engine.runAndWait()

    with wave.open(tmp_wav, 'rb') as wf:
        n_channels = wf.getnchannels()
        sampwidth = wf.getsampwidth()
        framerate = wf.getframerate()
        raw = wf.readframes(wf.getnframes())
        print(f"[WAV] channels={n_channels} sampwidth={sampwidth} rate={framerate}")

    if n_channels == 2:
        raw = audioop.tomono(raw, sampwidth, 0.5, 0.5)
    raw, _ = audioop.ratecv(raw, sampwidth, 1, framerate, 8000, None)
    if sampwidth != 2:
        raw = audioop.lin2lin(raw, sampwidth, 2)
    # Convert to 8-bit unsigned — half the bandwidth
    raw = audioop.lin2lin(raw, 2, 1)

    os.unlink(tmp_wav)
    print(f"[DEBUG] pcm={len(raw)}b")
    return raw

async def main():
    proc = subprocess.Popen(
        [NIOS2_TERMINAL, "--instance", "0"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        bufsize=0,
        creationflags=subprocess.CREATE_NO_WINDOW
    )

    buf = ""
    print("Connected. Waiting for DE1-SoC input...")

    try:
        while True:
            byte = await asyncio.get_event_loop().run_in_executor(
                None, proc.stdout.read, 1
            )
            if not byte:
                print("nios2-terminal closed.")
                break

            char = byte.decode("utf-8", errors="ignore")

            if char == "\n":
                line = buf.strip()
                buf = ""

                if not line or line.startswith("nios2") or line.startswith("Nios"):
                    continue

                print(f"[DE1-SoC] >>> {line}")
                full_response = []

                async for text_chunk in stream_llm_query(line):
                    print(f"[LLM] {text_chunk}", end="", flush=True)
                    full_response.append(text_chunk)
                    send_text(proc, text_chunk)  # stream each chunk as it arrives

                send_text_end(proc)  # signal end of text stream

                full_text = "".join(full_response)
                print(f"\n[TTS] Converting to audio...")
                pcm_data = await tts_to_pcm(full_text)
                if not pcm_data:
                    print("[ERROR] TTS produced no audio, skipping send")
                else:
                    print(f"[Audio] Sending {len(pcm_data)} samples to FPGA")
                    send_audio(proc, pcm_data)

            else:
                buf += char

    finally:
        proc.terminate()
        await asyncio.get_event_loop().run_in_executor(None, proc.wait)

if __name__ == "__main__":
    if sys.platform == "win32":
        asyncio.set_event_loop_policy(asyncio.WindowsProactorEventLoopPolicy())
    asyncio.run(main())