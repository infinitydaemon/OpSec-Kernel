import os
import paramiko
import getpass

def generate_snmp_config(filename):
    snmp_config_content = """
    # Example SNMP configuration
    snmp-server community public RO
    snmp-server contact admin@example.com
    """
    with open(filename, 'w') as f:
        f.write(snmp_config_content.strip())

def copy_file_with_sudo(source, destination, username, password):
    command = f"sudo cp {source} {destination}"
    ssh_client = paramiko.SSHClient()
    ssh_client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh_client.connect(hostname=destination, username=username, password=password)
    ssh_client.exec_command(command)
    ssh_client.close()

def main():
    filename = input("Enter the name of the SNMP configuration file to create: ")
    generate_snmp_config(filename)
    destination_computers = input("Enter destination computers separated by comma: ").split(',')
    username = input("Enter your username: ")
    password = getpass.getpass(prompt="Enter your password (will be hidden): ")

    for computer in destination_computers:
        response = os.system(f"ping -c 1 {computer} > /dev/null 2>&1")
        if response == 0:
            # Copy the file with sudo
            copy_file_with_sudo(filename, computer, username, password)
            print(f"SNMP configuration file copied to {computer}")
        else:
            print(f"Destination {computer} is not reachable.")
    os.remove(filename)
    print("Script completed successfully.")

if __name__ == "__main__":
    main()
