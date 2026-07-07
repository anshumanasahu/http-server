#!/bin/bash
mkdir -p certs
if [ ! -f certs/cert.pem ]; then
  echo "Generating self-signed certificate..."
  openssl req -x509 -newkey rsa:2048 -keyout certs/key.pem -out certs/cert.pem -days 365 -nodes -subj "/C=US/ST=State/L=City/O=Organization/CN=localhost"
  echo "Certificate generated in certs/"
else
  echo "Certificate already exists in certs/"
fi
