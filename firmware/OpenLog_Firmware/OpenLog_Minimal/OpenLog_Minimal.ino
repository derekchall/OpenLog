/*
 12-3-09
 Nathan Seidle
 SparkFun Electronics 2012

 OpenLog hardware and firmware are released under the Creative Commons Share Alike v3.0 license.
 This version has been heavily optimized for maximum RAM utilization, employing a custom bare-metal
 UART interrupt and a Zero-Copy buffer architecture to survive SD card write latencies.
 */

#define __PROG_TYPES_COMPAT__ 

#include <SPI.h>
#include <SdFat.h>  
#include <EEPROM.h>
#include <FreeStack.h> 

#include <avr/sleep.h>  
#include <avr/power.h>  
#include <avr/interrupt.h>

#define DEBUG 0
#define SD_CHIP_SELECT 10  //On OpenLog this is pin 10

// -------------------------------------------------------------------------
// CUSTOM BARE-METAL UART & ZERO-COPY BUFFER (Replaces SerialPort.h)
// -------------------------------------------------------------------------
// We allocate 1150 bytes. At 115200 baud, this can buffer ~100ms of continuous 
// data if the SD Card pauses for wear-leveling operations!
#define RX_BUFFER_SIZE 1150 
volatile uint8_t rx_buf[RX_BUFFER_SIZE];
volatile uint16_t rx_head = 0;
volatile uint16_t rx_tail = 0;

// The Hardware RX Interrupt: Fires automatically in the background when a byte arrives
ISR(USART_RX_vect) {
  uint8_t c = UDR0; // Read the hardware register
  uint16_t next_head = rx_head + 1;
  if (next_head == RX_BUFFER_SIZE) next_head = 0;
  
  // If buffer isn't full, save the character
  if (next_head != rx_tail) {
    rx_buf[rx_head] = c;
    rx_head = next_head;
  }
}

// Initializes the Hardware UART
void initUART(long baud) {
  uint16_t ubrr = (F_CPU / 8 / baud) - 1; // Double speed mode calc
  UCSR0A = (1 << U2X0); 
  UBRR0H = (uint8_t)(ubrr >> 8);
  UBRR0L = (uint8_t)ubrr;
  // Enable RX, TX, and the RX Interrupt
  UCSR0B = (1 << RXEN0) | (1 << TXEN0) | (1 << RXCIE0);
  UCSR0C = (1 << UCSZ01) | (1 << UCSZ00); // 8-bit data
}

// Transmit a single character out the TX pin
void uart_putchar(char c) {
  while (!(UCSR0A & (1 << UDRE0))); // Wait for empty transmit buffer
  UDR0 = c;
}

// Helper functions to print strings (replaces Arduino's Serial.print)
void uart_print(const char* str) {
  while (*str) uart_putchar(*str++);
}
void uart_print_P(const char PROGMEM* str) {
  char c;
  while ((c = pgm_read_byte(str++))) uart_putchar(c);
}
void uart_print_int(int val) {
  char buf[8];
  itoa(val, buf, 10);
  uart_print(buf);
}
// -------------------------------------------------------------------------

//Internal EEPROM locations for the user settings
#define LOCATION_FILE_NUMBER_LSB 0x03
#define LOCATION_FILE_NUMBER_MSB 0x04

//STAT1 is a general LED and indicates serial traffic
const byte stat1 = 5;   
const byte stat2 = 13;  

//Blinking LED error codes
#define ERROR_SD_INIT 3
#define ERROR_CARD_INIT 6
#define ERROR_VOLUME_INIT 7
#define ERROR_ROOT_INIT 8
#define ERROR_FILE_OPEN 9

SdFat sd;
long setting_uart_speed = 115200;

void systemError(byte errorType) {
  uart_print_P(PSTR("Error "));
  switch (errorType) {
    case ERROR_CARD_INIT:
      uart_print_P(PSTR("card.init"));
      blinkError(ERROR_SD_INIT);
      break;
    case ERROR_VOLUME_INIT:
      uart_print_P(PSTR("volume.init"));
      blinkError(ERROR_SD_INIT);
      break;
    case ERROR_ROOT_INIT:
      uart_print_P(PSTR("root.init"));
      blinkError(ERROR_SD_INIT);
      break;
    case ERROR_FILE_OPEN:
      uart_print_P(PSTR("file.open"));
      blinkError(ERROR_SD_INIT);
      break;
  }
}

void setup(void) {
  pinMode(stat1, OUTPUT);

  set_sleep_mode(SLEEP_MODE_IDLE);
  sleep_enable();

  //Shut off TWI, Timer2, Timer1, ADC
  ADCSRA &= ~(1 << ADEN);               
  ACSR = (1 << ACD);                    
  DIDR0 = 0x3F;                         
  DIDR1 = (1 << AIN1D) | (1 << AIN0D);  

  power_twi_disable();
  power_timer1_disable();
  power_timer2_disable();
  power_adc_disable();

  // Setup our custom UART
  initUART(setting_uart_speed);
  uart_print_P(PSTR("1"));

  //Setup SD & FAT
  if (!sd.begin(SD_CHIP_SELECT, SPI_FULL_SPEED)) systemError(ERROR_CARD_INIT);

  uart_print_P(PSTR("2"));

#if DEBUG
  uart_print_P(PSTR("\n\nFreeStack: "));
  uart_print_int(FreeStack());
  uart_print_P(PSTR("\r\n"));
#endif
}

void loop(void) {
  appendFile(newLog());  
}

char* newLog(void) {
  byte msb, lsb;
  unsigned int newFileNumber;

  SdFile newFile;  

  lsb = EEPROM.read(LOCATION_FILE_NUMBER_LSB);
  msb = EEPROM.read(LOCATION_FILE_NUMBER_MSB);

  newFileNumber = msb;
  newFileNumber = newFileNumber << 8;
  newFileNumber |= lsb;

  if ((lsb == 255) && (msb == 255)) {
    newFileNumber = 0; 
    EEPROM.write(LOCATION_FILE_NUMBER_LSB, 0x00);
    EEPROM.write(LOCATION_FILE_NUMBER_MSB, 0x00);
  }

  if (newFileNumber == 65534) {
    uart_print_P(PSTR("!Too many logs:1!"));
    return (0); 
  }

  static char newFileName[13];
  while (1) {
    sprintf_P(newFileName, PSTR("LOG%05d.TXT"), newFileNumber); 

    if (newFile.open(newFileName, O_CREAT | O_EXCL | O_WRITE)) break;

    if (newFile.open(newFileName, O_READ)) {
      if (newFile.fileSize() == 0) {
        newFile.close();       
        return (newFileName);  
      }
      newFile.close(); 
    }

    newFileNumber++;
    if (newFileNumber > 65533) {
      uart_print_P(PSTR("!Too many logs:2!"));
      return (0);  
    }
  }

  newFileNumber++;  

  lsb = (byte)(newFileNumber & 0x00FF);
  msb = (byte)((newFileNumber & 0xFF00) >> 8);

  EEPROM.write(LOCATION_FILE_NUMBER_LSB, lsb);  

  if (EEPROM.read(LOCATION_FILE_NUMBER_MSB) != msb)
    EEPROM.write(LOCATION_FILE_NUMBER_MSB, msb); 

  return (newFileName);
}

//Appends a stream of serial data to a given file using ZERO-COPY
byte appendFile(char* fileName) {
  SdFile workingFile;

  if (!workingFile.open(fileName, O_CREAT | O_APPEND | O_WRITE)) systemError(ERROR_FILE_OPEN);

  if (workingFile.fileSize() == 0) {
    workingFile.rewind();
    workingFile.sync();
  }

  const unsigned int MAX_IDLE_TIME_MSEC = 500;  
  digitalWrite(stat1, HIGH);  

  unsigned long lastSyncTime = millis();  
  
  byte triggerState = 0; 
  bool echoing = false;  

  while (1) {
    
    // Safely read the 16-bit head pointer (must pause interrupts for 1 microsecond to read safely)
    uint16_t current_head;
    cli(); 
    current_head = rx_head;
    sei();

    if (current_head != rx_tail) {
      toggleLED(stat1);  
      
      // Calculate how many contiguous bytes we can read before the ring buffer wraps around
      uint16_t chunk_len;
      if (current_head > rx_tail) {
        chunk_len = current_head - rx_tail; // Direct straight line
      } else {
        chunk_len = RX_BUFFER_SIZE - rx_tail; // Read up to the physical end of the array
      }

      // 1. Process Trigger Logic over this contiguous block
      for (uint16_t i = 0; i < chunk_len; i++) {
        char c = (char)rx_buf[rx_tail + i];
        
        if (!echoing) {
          if (triggerState == 0 && c == 'A') triggerState = 1;
          else if (triggerState == 1 && c == 'T') triggerState = 2;
          else if (triggerState == 2 && c == '+') {
            triggerState = 0; 
            echoing = true;
            uart_putchar('A'); uart_putchar('T'); uart_putchar('+');
          } else {
            if (c == 'A') triggerState = 1;
            else triggerState = 0;
          }
        } else {
          uart_putchar(c);
          if (c == '\n' || c == '\r') echoing = false;
        }
      }
      
      // 2. ZERO-COPY WRITE: Hand the exact RAM address of the RX buffer directly to the SD Card!
      workingFile.write((const uint8_t*)&rx_buf[rx_tail], chunk_len);

      // 3. Update the tail pointer safely
      uint16_t next_tail = rx_tail + chunk_len;
      if (next_tail == RX_BUFFER_SIZE) next_tail = 0; // Wrap around if we hit the end
      
      cli();
      rx_tail = next_tail;
      sei();

      lastSyncTime = millis(); // Reset sync timer
    }

    else if ((unsigned long)(millis() - lastSyncTime) > MAX_IDLE_TIME_MSEC) {
      workingFile.sync();  
      digitalWrite(stat1, LOW);  

      power_timer0_disable();  
      power_spi_disable();
      sleep_mode();  // Go to sleep, wake instantly when hardware interrupt fires

      power_spi_enable();  
      power_timer0_enable();
      lastSyncTime = millis();  
    }
  }

  return (1);  
}

//=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=-=
void blinkError(byte ERROR_TYPE) {
  while (1) {
    for (int x = 0; x < ERROR_TYPE; x++) {
      digitalWrite(stat1, HIGH);
      delay(200);
      digitalWrite(stat1, LOW);
      delay(200);
    }
    delay(2000);
  }
}

void toggleLED(byte pinNumber) {
  if (digitalRead(pinNumber)) digitalWrite(pinNumber, LOW);
  else digitalWrite(pinNumber, HIGH);
}