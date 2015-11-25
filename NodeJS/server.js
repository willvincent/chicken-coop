var mosca      = require('mosca')
 ,  yaml       = require('read-yaml')
 ,  express    = require('express')
 ,  Primus     = require('primus')
 ,  app        = express()
 ,  moment     = require('moment')
 ,  _          = require('lodash')
 ,  path       = require('path');

var config = yaml.sync(path.resolve(__dirname, 'config.yml'));
var socket = null;
var tzOffset = (new Date().getTimezoneOffset() * 60) * -1;

// Sends time information every 'beaconInterval' seconds
var timeBeacon = function() {
  var message = {
    topic: 'time/beacon',
    payload: (parseInt(moment().format('X')) + tzOffset).toString(),
    qos: 0,
    retain: false
  };

  mqttServer.publish(message);
};
setInterval(timeBeacon, (config.beacon.interval * 1000));

// Load DB config
app.set('db', require('./models'));

// Ensure default statuses are present.
// This will set default values if that table was missing, or not
// fully populated when the server was started. Really only necessary on
// first run of the server.
var Status = app.get('db').Status;
Status.sync().then(function () { // Ensure table exists before attempting findAll().
  Status.findAll({
    order: [ ['id', 'ASC'] ]
  }).then(function (statuses) {
    for (var id in config.statuses) {
      var index = _.findKey(statuses, {id: id});
      if (typeof index === 'undefined') {
        Status.create({ id: id, status: config.statuses[id] });
      }
    }
  });
});

// Configure sunrise/set data
var sunRise = config.sun.default.rise;
var sunSet  = config.sun.default.Set;
if (config.sun.wunderground.enable) {
  var sunStatus = function() {
    var http = require('http');
    var options = {
      host: 'api.wunderground.com',
      path: '/api/' + config.sun.wunderground.key + '/astronomy/q/' + config.sun.wunderground.state + '/' + config.sun.wunderground.city + '.json'
    };

    var wuCb = function (res) {
      if (res.statusCode == 200) {
        var str = '';
        res.on('data', function (chunk) {
          str += chunk;
        });
        res.on('end', function() {
          var data = JSON.parse(str);

          if (typeof data.sun_phase !== 'undefined') {
            sunRise = parseInt(data.sun_phase.sunrise.hour) + 1;
            sunSet  = parseInt(data.sun_phase.sunset.hour) - 1;
          }
          var msgA = {
            topic: 'sun/rise',
            payload: sunRise.toString(),
            qos: 2,
            retain: false
          };

          var msgB = {
            topic: 'sun/set',
            payload: sunSet.toString(),
            qos: 2,
            retain: false
          };

          mqttServer.publish(msgA);
          mqttServer.publish(msgB);
        });
      }
    };

    var req = http.request(options);
    req.end();
  };
  setInterval(sunStatus, 43200000); // Every 12hrs
}

var mqttServer = mosca.Server(config.mqtt);
app.express = express;
require('./routes')(app);

// Setup static file path
app.use(express.static(__dirname + '/public'));

// Force all requests not to a defined route thru the static index.html file
// this is necessary for html5routes to work properly with angular.
app.use(function(req, res) {
  res.sendfile(path.normalize(__dirname + '/public/index.html'));
});

var server = app.listen(config.web.port, config.web.host, function() {
  console.log('Webserver listening at http://%s:%s', config.web.host, config.web.port);
});

/*******************
 ***  MQTT stuff ***
 *******************/

mqttServer.on('ready', function() {
  mqttServer.authenticate = function(client, username, password, callback) {
    var authorized = (client.id === config.mqtt.client.id && username === config.mqtt.client.user && password.toString() === config.mqtt.client.pass);
    if (authorized) {
      client.user = username;
    }
    callback(null, authorized);
  };
  console.log('MQTT Broker listening at %s on port %s', config.mqtt.host, config.mqtt.port);
});

mqttServer.on('clientConnected', function(client) {
  if (client.id == config.mqtt.client.id) {
    primus.write({
      clientStatus: 'Online'
    });
  }
  mqttServer.publish({topic: 'sun/rise', payload: sunRise.toString(), qos: 2, retain: false});
  mqttServer.publish({topic: 'sun/set', payload: sunSet.toString(), qos: 2, retain: false});
});

mqttServer.on('clientDisconnected', function(client) {
  if (client.id == config.mqtt.client.id) {
    primus.write({
      clientStatus: 'Offline'
    });
  }
});

mqttServer.on('published', function(packet, client) {
  switch(packet.topic) {
    case 'coop/temperature':
      var Temperature = app.get('db').Temperature;
      Temperature.create({ reading: parseFloat(packet.payload) });
      primus.write({
        update: {
          temp: parseFloat(packet.payload)
        }
      });
      break;
    case 'coop/brightness':
      var Brightness = app.get('db').Brightness;
      Brightness.create({ reading: parseInt(packet.payload) });
      primus.write({
        update: {
          light: parseInt(packet.payload)
        }
      });
      break;
    case 'coop/status':
      var Status = app.get('db').Status;
      var data = packet.payload.toString().split('|');
      Status.upsert({ id: data[0], status: data[1] });
      primus.write({
        update: {
          name: data[0],
          status: data[1],
          updated: parseInt(moment().format('X'))
        }
      });
      break;
  }
});


/*********************
 ***  Socket stuff ***
 *********************/

var primus = new Primus(server, { transformer: 'socket.io' });

primus.on('connection', function (spark) {
  // Hydrate brightness data
  var Brightness = app.get('db').Brightness;
  Brightness.findAll({
    where: {
      createdAt: {
        $gt: moment().subtract(24, 'hours').format('YYYY-MM-DD HH:mm:ss')
      }
    },
    order: [ ['createdAt', 'ASC'] ],
    limit: 360
  }).then(function(results) {
    var data = [];
    results.forEach(function(result) {
      data.push({
        createdAt: parseInt(moment(result.dataValues.createdAt).format('x')),
        reading: result.dataValues.reading
      });
    });
    spark.write({ lightReadings: data });
  });

  // Hydrate temperature data
  var Temperature = app.get('db').Temperature;
  Temperature.findAll({
    where: {
      createdAt: {
        $gt: moment().subtract(24, 'hours').format('YYYY-MM-DD HH:mm:ss')
      }
    },
    order: [ ['createdAt', 'ASC'] ],
    limit: 360
  }).then(function(results) {
    var data = [];
    results.forEach(function(result) {
      data.push({
        createdAt: parseInt(moment(result.dataValues.createdAt).format('x')),
        reading: result.dataValues.reading
      });
    });
    spark.write({ tempReadings: data });
  });

  // Hydrate status data
  var Status = app.get('db').Status;
  Status.findAll({
    order: [ ['id', 'ASC'] ]
  }).then(function(results) {
    var data = [];
    results.forEach(function(result) {
      data.push({
        name: result.dataValues.id,
        status: result.dataValues.status,
        updated: parseInt(moment(result.dataValues.updatedAt).format('X'))
      });
    });
    spark.write({ statuses: data });
  });

  // Handle remote trigger events (handoff from socket to mqtt)
  spark.on('data', function(data) {
    if (typeof data.remoteTrigger != 'undefined') {
      var message = {
        topic: 'coop/remotetrigger',
        payload: data.remoteTrigger,
        qos: 2,
        retain: false
      };

      mqttServer.publish(message);
    }
  })
});
