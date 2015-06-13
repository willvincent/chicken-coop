var bodyParser = require('body-parser')
 ,  yaml       = require('read-yaml');

var config = yaml.sync('config.yml');

var jsonParser = bodyParser.json();
module.exports = function(app) {
  app.post('/api/authenticate', jsonParser, function(req, res) {
    if (req.body.password === config.admin.password && req.body.username === config.admin.username) {
      res.send({success:true}, 200);
    }
    else {
      res.send({success:false}, 403);
    }
    res.end();
  })
};
