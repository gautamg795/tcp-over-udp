# -*- mode: ruby -*-
# vi: set ft=ruby :

$INSTALL_BASE = <<SCRIPT
    sudo apt-get update
    sudo apt-get upgrade -y
    sudo apt-get install -y build-essential gdb vim valgrind
SCRIPT

Vagrant.configure(2) do |config|
  config.vm.box = "ubuntu/trusty64"
  config.vm.provision "shell", inline: $INSTALL_BASE
  config.vm.define :client, primary: true do |host|
      host.vm.hostname = "client"
      host.vm.network "private_network", ip: "10.0.0.2", netmask: "255.255.255.0"
      host.vm.provision "shell", inline: "tc qdisc add dev eth1 root netem loss 10% delay 20ms"
  end
  config.vm.define :server do |host|
      host.vm.hostname = "server"
      host.vm.network "private_network", ip: "10.0.0.1", netmask: "255.255.255.0"
      host.vm.provision "shell", inline: "tc qdisc add dev eth1 root netem loss 10% delay 20ms"
  end
end
