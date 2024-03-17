# pip install pysnmp
from pysnmp.hlapi import *

def get_snmp_info(ip_address, community='public', port=161, oid='1.3.6.1.2.1.1.1.0'):
    errorIndication, errorStatus, errorIndex, varBinds = next(
        getCmd(SnmpEngine(),
               CommunityData(community),
               UdpTransportTarget((ip_address, port)),
               ContextData(),
               ObjectType(ObjectIdentity(oid)))
    )

    if errorIndication:
        print(f"Error in communication with {ip_address}: {errorIndication}")
        return None
    elif errorStatus:
        print(f"Error in response from {ip_address}: {errorStatus.prettyPrint()}")
        return None
    else:
        for varBind in varBinds:
            return varBind[1].prettyPrint()


if __name__ == "__main__":
    # Define a list of IP addresses of devices in your LAN
    devices = ['192.168.1.1', '192.168.1.2', '192.168.1.3']

    # Loop through each device and retrieve SNMP information
    for device_ip in devices:
        print(f"SNMP Info for device at {device_ip}:")
        sys_descr = get_snmp_info(device_ip)
        if sys_descr:
            print(f"System Description: {sys_descr}")
        else:
            print("Failed to retrieve SNMP information.")
