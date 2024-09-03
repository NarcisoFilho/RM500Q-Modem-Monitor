default:
	gcc -o modem_monitor main.c
run:
	$(MAKE) build
	sudo ./modem_monitor -c config.txt
clean:
	rm modem_monitor