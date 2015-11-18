# NodeJS server
This portion of the code is for a node.js powered server to push chicken coop status and statistics to.
It also provides a nice front-end to view real-time status and statistics of the coop, including;
- Temperature and light level real-time graphs
- On/off indications for coop light and water heater
- Open/Closed indication for coop door


## Installation & Usage

1. Install required packages with NPM:

   ```
   npm install
   ```

2. Edit `config.yml` setting appropriate database credentials, secure admin credentials, and default status items and values. Other settings should be adjusted here as needed.

3. Create SQL database as configured in the config.yml file, preferably with either sqlite or mysql.

4. Run the server with node:

   ```
   node server.js
   ```

   It would be preferable to run this with a utility like [Forever](https://www.npmjs.com/package/forever) to keep the server running, in the unlikely event that it should crash. You may also want to set it up as a service so that it starts and stop automatically when your server is restarted. That configuration is beyond the scope of this project, but [this article](https://www.exratione.com/2011/07/running-a-nodejs-server-as-a-service-using-forever/) should get you pointed in the right direction.
