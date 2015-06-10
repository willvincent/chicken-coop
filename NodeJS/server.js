var mosca      = require('mosca')
 ,  yaml       = require('read-yaml')
 ,  express    = require('express')
 ,  app        = express()
 ,  path       = require('path');

var config = yaml.sync('config.yml');

app.set('db', require('./models'));

var mqttServer = mosca.Server(config.mqtt);

require('./routes')(app, express);

app.use(express.static(__dirname + '/public'));

// Force all requests not to a defined route thru the static index.html file
// this is necessary for html5routes to work properly with angular.
app.use(function(req, res) {
  res.sendfile(path.normalize(__dirname + '/public/index.html'));
});

mqttServer.on('ready', function() {
  console.log('MQTT Broker listening on port %s', config.mqtt.port);
});

var server = app.listen(config.web.port, config.web.host, function() {
  console.log('Webserver listening at http://%s:%s', config.web.host, config.web.port);
});
