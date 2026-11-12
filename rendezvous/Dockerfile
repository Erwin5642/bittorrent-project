# Servidor de rendezvous (TCP, protocolo de texto; ver README.md).
# Build (contexto = esta pasta): docker build -t bt-rendezvous rendezvous/
# Local: docker run --rm --network host -e PORT=7000 bt-rendezvous
#        docker run --rm -p 7000:7000 bt-rendezvous -t 120
#
# Imagem base fixada por digest (alpine 3.20).
ARG ALPINE_IMAGE=alpine:3.20@sha256:c64c687cbea9300178b30c95835354e34c4e4febc4badfe27102879de0483b5e

FROM ${ALPINE_IMAGE} AS build
RUN apk add --no-cache gcc musl-dev make
WORKDIR /src
COPY rendezvous.c Makefile ./
RUN make

FROM ${ALPINE_IMAGE}
# Usuário não-root com UID/GID numéricos fixos
RUN addgroup -S -g 1001 rdv && adduser -S -D -H -u 1001 -G rdv rdv
COPY --from=build /src/rendezvous /usr/local/bin/rendezvous
USER 1001:1001

# A porta vem de PORT (padrão 7000)
ENV PORT=7000
EXPOSE 7000

# O servidor atende PING com PONG (busybox nc, sem dependências extras)
HEALTHCHECK --interval=30s --timeout=3s --start-period=3s --retries=3 \
  CMD sh -c 'echo PING | nc -w 2 127.0.0.1 "$PORT" | grep -q PONG'

ENTRYPOINT ["rendezvous"]
CMD ["-t", "60", "-v"]
