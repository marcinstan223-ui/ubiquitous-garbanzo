FROM ubuntu:latest

# Instalujemy kompilator GCC i wymagane biblioteki
RUN apt-get update && apt-get install -y gcc make build-essential

# Kopiujemy nasz kod do kontenera
WORKDIR /app
COPY bot_client_vps.c .

# Kompilujemy bota (-pthread jest wymagane, bo użyliśmy wątków)
RUN gcc bot_client_vps.c -o bot_vps -pthread

# Uruchamiamy skurczybyka
CMD ["./bot_vps"]
