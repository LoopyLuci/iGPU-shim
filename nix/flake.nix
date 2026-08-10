{
  "description": "Synapse iGPU Shim — local runner flake for NixOS";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
      in
      {
        devShells.default = pkgs.mkShell {
          buildInputs = with pkgs; [
            cmake
            ninja
            gcc
            gdb
            valgrind
            qemu
            OVMF
            virglrenderer
            spice
            win-spice
            wget
            curl
            unzip
            zip
            python3
            python3Packages.pyyaml
            git
          ];

          shellHook = ''
            echo "Synapse local runner environment ready."
            echo "Native build:     build-native"
            echo "Windows VM build: build-windows-vm"
            echo "Run tests:        test-native"
          '';

          # Helper scripts
          build-native = ''
            echo "[native] Building Synapse..."
            mkdir -p build_native
            cd build_native
            cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
            ninja
            ctest --output-on-failure
          '';

          build-windows-vm = ''
            echo "[windows-vm] Building Synapse in Windows VM..."
            echo "Prerequisites:"
            echo "  1. Install qemu, OVMF, and Windows 10/11 ISO"
            echo "  2. Create VM with: nix run .#create-windows-vm"
            echo "  3. Start VM:        nix run .#start-windows-vm"
            echo "  4. Inside VM:       .\\build_msvc.bat Release stub"
          '';
        };

        packages.default = pkgs.hello;

        # NixOS module for local CI runner service
        nixosModules.runner = { config, lib, ... }:
          let
            cfg = config.services.synapse-runner;
          in
          {
            options.services.synapse-runner = {
              enable = lib.mkEnableOption "Synapse local CI runner";

              listenPort = lib.mkOption {
                type = lib.types.int;
                default = 9876;
                description = "Port for local runner HTTP API";
              };

              workDir = lib.mkOption {
                type = lib.types.path;
                default = "/var/lib/synapse-runner";
                description = "Working directory for runner jobs";
              };

              allowedHosts = lib.mkOption {
                type = lib.types.listOf lib.types.str;
                default = [ "localhost" "127.0.0.1" "::1" ];
                description = "Allowed hosts for runner API";
              };
            };

            config = lib.mkIf cfg.enable {
              # Firewall rules
              networking.firewall = {
                allowedTCPPorts = [ cfg.listenPort ];
                allowedUDPPorts = [ cfg.listenPort ];
              };

              # Systemd service for local runner
              systemd.services.synapse-runner = {
                description = "Synapse Local CI Runner";
                wantedBy = [ "multi-user.target" ];
                after = [ "network.target" ];

                serviceConfig = {
                  Type = "simple";
                  WorkingDirectory = cfg.workDir;
                  ExecStart = "${pkgs.python3}/bin/python3 ${./scripts/runner.py} --port ${toString cfg.listenPort}";
                  Restart = "always";
                  RestartSec = 5;
                };

                environment = {
                  SYNAPSE_RUNNER_MODE = "local";
                  SYNAPSE_RUNNER_HOSTS = lib.concatStringsSep "," cfg.allowedHosts;
                };
              };

              # Create work directory
              systemd.tmpfiles.rules = [
                "d ${cfg.workDir} 0750 synapserunner synapserunner"
              ];

              users.users.synapserunner = {
                group = "synapserunner";
                isSystemUser = true;
              };
              users.groups.synapserunner = {};
            };
          };
      }
}
