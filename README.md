# Zapshare

This is a simple file sharing application that uses a `cli tool` and a `rendezvous` server to share files

`sender --> server <--> client`

The sender sends file metadata and connection data to the server and when the client does a `get` this metadata is given to the client and then the client can use that connection data to connect to the sender and begin the transfer of file.

## Usage

For sending the file:
<br>`zapshare send <file_path>`

This will generate a secret hash that has to be shared with the receiver to get the file.

To get the file from the sender:
<br>`zapshare get <secret>`

This should start the transfer after connecting to the client and save the file to your `current working directory`.

#### <u>This project currently only works for peers on the same network as workarounds for NAT are not done.</u>

## Upcoming changes

I'll be implementing UDP hole punching to make sure the peers on different networks are able to share data.
