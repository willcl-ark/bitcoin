Bitcoin Core
=============

Intro
-----
Bitcoin is a free open source peer-to-peer electronic cash system that is
completely decentralized, without the need for a central server or trusted
parties.  Users hold the crypto keys to their own money and transact directly
with each other, with the help of a P2P network to check for double-spending.


Setup
-----
- Unpack the files into a directory.

It may be necessary to codesign the binaries to bypass Gatekeeper on MacOS >=
10.15, otherwise they will not run and will suggest you move them to the Trash.

The provided helper script "codesign_macos.sh" will attempt to perform this for
you, however you should check the contents of this script before running it.

- If you are happy with the script contents, you can run "./codesign_macos.sh"
  to perform the codesigning.

- Run "bitcoind"

Bitcoin Core is the original Bitcoin client and it builds the backbone of the network.
However, it downloads and stores the entire history of Bitcoin transactions;
depending on the speed of your computer and network connection, the synchronization
process can take anywhere from a few hours to a day or more.

See the bitcoin wiki at:
  https://en.bitcoin.it/wiki/Main_Page
for more help and information.
