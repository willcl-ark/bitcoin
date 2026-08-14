{ pkgs, bitcoinCore }:

let
  bitcoinUid = 101;
  bitcoinGid = 101;
  bitcoinHome = "/home/bitcoin";
  bitcoinData = "${bitcoinHome}/.bitcoin";
in
pkgs.dockerTools.buildLayeredImage {
  name = "bitcoin-core";
  tag = "latest";
  architecture = "amd64";
  contents = [ bitcoinCore ];
  fakeRootCommands = ''
    mkdir -p ./home/bitcoin/.bitcoin
    chmod 0755 ./home ./home/bitcoin
    chmod 0700 ./home/bitcoin/.bitcoin
    chown -R ${toString bitcoinUid}:${toString bitcoinGid} ./home/bitcoin
  '';
  config = {
    Entrypoint = [ "/bin/bitcoind" ];
    Cmd = [ "-printtoconsole" ];
    User = "${toString bitcoinUid}:${toString bitcoinGid}";
    Env = [
      "BITCOIN_DATA=${bitcoinData}"
      "HOME=${bitcoinHome}"
      "PATH=/bin"
    ];
    WorkingDir = bitcoinHome;
    Volumes.${bitcoinData} = { };
    ExposedPorts = {
      "8333/tcp" = { };
      "18333/tcp" = { };
      "18444/tcp" = { };
      "38333/tcp" = { };
      "48333/tcp" = { };
    };
    StopSignal = "SIGTERM";
  };
}
