FROM python:3.12-slim
WORKDIR /src
COPY pyproject.toml README.md ./
COPY molty_voice molty_voice
RUN pip install --no-cache-dir '.[reflex]'
ENTRYPOINT ["molty-voice"]
