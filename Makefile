default:
	gcc -o modem_monitor main.c
driving:
	gcc -o driving_robot.c driving_robot
run:
	$(MAKE) build
	sudo ./modem_monitor -c config.txt
clean:
	rm modem_monitor