# Signaling server only. The game itself is a terminal program and has no
# business in a container; this is the one piece that runs unattended on a
# host with a public address.
#
#   docker build -t warship-rendezvous ./server
#   docker run -d --restart=unless-stopped -p 7777:7777 warship-rendezvous
#
# rendezvous.c deliberately links nothing from the rest of the repo -- no
# libjuice, no libplum, no game code -- so the build context is this directory
# and the final image needs no runtime at all.

FROM alpine:3.20 AS build
RUN apk add --no-cache gcc musl-dev
WORKDIR /src
COPY rendezvous.c .
RUN gcc -Wall -Wextra -Werror -std=c11 -O2 -static -o rendezvous rendezvous.c

FROM scratch
COPY --from=build /src/rendezvous /rendezvous
# Numeric uid: there is no /etc/passwd in the image to resolve a name against.
USER 65534:65534
EXPOSE 7777/tcp
ENTRYPOINT ["/rendezvous"]
CMD ["7777"]
