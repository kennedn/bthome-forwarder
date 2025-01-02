package main

import (
	"encoding/hex"
	"fmt"
	"log"
    "strings"

	"github.com/gorilla/websocket"
)

const bthomeUUID = "FCD2" // BTHome UUID reversed

// Measurement represents a parsed measurement from the BTHome message
type Measurement struct {
	Type  string
	Value string
	Unit  string
}

func main() {
	url := "ws://192.168.1.164" // Replace with your WebSocket URL
	conn, _, err := websocket.DefaultDialer.Dial(url, nil)
	if err != nil {
		log.Fatalf("Failed to connect to WebSocket: %v", err)
	}
	defer conn.Close()

	log.Println("Connected to WebSocket. Waiting for messages...")

	for {
		_, message, err := conn.ReadMessage()
		if err != nil {
			log.Printf("Error reading message: %v", err)
			continue
		}
		
		hexString := string(message)
		data, err := hex.DecodeString(hexString)
		if err != nil {
			log.Printf("Invalid hex string: %v", err)
			continue
		}

        parseBTHomeData(formatWithColons(hexString[:12]), data[6:])
	}
}

func formatWithColons(input string) string {
	if len(input)%2 != 0 {
		// Return an error message if the input is not even
		return "Input length must be even"
	}

	var parts []string
	for i := 0; i < len(input); i += 2 {
		parts = append(parts, input[i:i+2])
	}

	return strings.Join(parts, ":")
}


func parseBTHomeData(address string, data []byte) {
	uuid := fmt.Sprintf("%02X%02X", data[1], data[0])
	if uuid != bthomeUUID {
		log.Printf("Unknown UUID: %v", data)
		return
	}

	deviceInfo := data[2]
	bthomeVersion := (deviceInfo >> 5) & 0x07
	if bthomeVersion != 2 {
		log.Printf("Unsupported BTHome version: %d", bthomeVersion)
		return
	}

	measurements := data[3:]
	parsedMeasurements := parseMeasurements(measurements)
    log.Printf("\n\tAddress: %s\n", address)
	for _, m := range parsedMeasurements {
        fmt.Printf("\t%s:  %s%s\n", m.Type, m.Value, m.Unit)
	}
}

func parseMeasurements(data []byte) []Measurement {
	var measurements []Measurement
	i := 0

	for i < len(data) {
		if len(data)-i < 2 {
			log.Println("Incomplete measurement data")
			break
		}

		objectID := data[i]
		i++

		var measurement Measurement
		var value interface{}
		var length int

		switch objectID {
        case 0x00: // packet id
			if len(data)-i < 1 {
				log.Println("Incomplete data")
				break
			}
			value = uint8(data[i])
			measurement = Measurement{Type: "Packet", Value: fmt.Sprintf("%d", value), Unit: ""}
			length = 1
        case 0x01: // battery 
            if len(data)-i < 1 {
                log.Println("Incomplete data")
                break
            }
            value = uint8(data[i])
            measurement = Measurement{Type: "Battery", Value: fmt.Sprintf("%d", value), Unit: "%"}
			length = 1
		case 0x02: // Temperature
			if len(data)-i < 2 {
				log.Println("Incomplete data")
				break
			}
			value = int16(data[i]) | int16(data[i+1])<<8
			measurement = Measurement{Type: "Temperature", Value: fmt.Sprintf("%.2f", float64(value.(int16))*0.01), Unit: "°C"}
			length = 2
		case 0x03: // Humidity
			if len(data)-i < 2 {
				log.Println("Incomplete data")
				break
			}
			value = uint16(data[i]) | uint16(data[i+1])<<8
			measurement = Measurement{Type: "Humidity", Value: fmt.Sprintf("%.2f", float64(value.(uint16))*0.01), Unit: "%"}
			length = 2
		case 0x0C: // voltage
			if len(data)-i < 2 {
				log.Println("Incomplete data")
				break
			}
			value = uint16(data[i]) | uint16(data[i+1])<<8
			measurement = Measurement{Type: "Voltage", Value: fmt.Sprintf("%.2f", float64(value.(uint16))*0.001), Unit: "V"}
			length = 2
		default:
			log.Printf("Unsupported object ID: 0x%X", objectID)
		}

		measurements = append(measurements, measurement)
		i += length
	}

	return measurements
}
