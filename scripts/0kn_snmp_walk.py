from pysnmp.hlapi import *

def snmp_walk(network, community='public', oid='1.3.6.1.2.1'):
    iterator = getCmd(SnmpEngine(),
                      CommunityData(community),
                      UdpTransportTarget((network, 161)),
                      ContextData(),
                      ObjectType(ObjectIdentity(oid)),
                      lexicographicMode=False)

    for errorIndication, errorStatus, errorIndex, varBinds in iterator:
        if errorIndication:
            print(f"Error: {errorIndication}")
            break
        elif errorStatus:
            print(f"Error: {errorStatus}")
            break
        else:
            for varBind in varBinds:
                print(f"{varBind[0]} = {varBind[1].prettyPrint()}")

if __name__ == "__main__":
    network = input("Enter the network address in CIDR notation (e.g., 192.168.1.0/24): ")
    community = input("Enter the SNMP community string: ")

    print(f"\nPerforming SNMP walk on network {network} with community string {community}...\n")
    snmp_walk(network, community)
