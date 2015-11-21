var app = angular.module('ArduinoChickenCoop', ['ngMaterial', 'ngAnimate', 'highcharts-ng', 'angularMoment']);

app.controller('AppCtrl', ['$scope', '$http', '$mdSidenav', '$window', '$filter', '$mdSidenav', '$mdUtil', function($scope, $http, $mdSidenav, $window, $filter, $mdSidenav, $mdUtil) {

  $scope.primus = Primus.connect();
  $scope.admin = false;

  $scope.items = [];
  $scope.currentLight = '---';
  $scope.currentTemp = '---';
  $scope.tempData = [];
  $scope.lightData = [];
  $scope.windowWidth = $window.innerWidth;
  $scope.clientStatus = "Offline";
  $scope.username = null;
  $scope.password = null;

  $scope.safeApply = function(fn) {
    var phase = this.$root.$$phase;
    if(phase == '$apply' || phase == '$digest') {
      if(fn && (typeof(fn) === 'function')) {
        fn();
      }
    } else {
      this.$apply(fn);
    }
  };

  $scope.toggleRight = $mdUtil.debounce(function(){$mdSidenav('right').toggle();},300);

  $scope.triggerItem = function(id) {
    console.log($scope.items[id]);
    if ($scope.items[id].name == 'door') {
      if ($scope.items[id].status == 'open') {
        $scope.items[id].status = 'opening';
      }
      if ($scope.items[id].status == 'closed') {
        $scope.items[id].status = 'closing';
      }
    }
    $scope.primus.write({remoteTrigger:$scope.items[id].name});
  }

  $scope.logout = function() {
    $scope.admin = false;
  };

  $scope.loginSubmit = function() {
    $http.post('/api/authenticate', { password: $scope.password, username: $scope.username })
    .success(function(data, status, headers, config) {
      $scope.admin = true;
      $scope.username = null;
      $scope.password = null;
      $scope.toggleRight();
      $scope.safeApply();
    })
    .error(function(data, status, headers, config) {
      $scope.admin = false;
      $scope.username = null;
      $scope.password = null;
      $scope.toggleRight();
      $scope.safeApply();
    });
  }

  $scope.primus.on('data', function message(data) {
    if (typeof data.statuses != 'undefined') {
      $scope.items = data.statuses;
    }
    if (typeof data.clientStatus != 'undefined') {
      $scope.clientStatus = data.clientStatus;
    }
    if (typeof data.lightReadings != 'undefined') {
      data.lightReadings.forEach(function(reading) {
        $scope.lightData.push([reading.createdAt, reading.reading]);
      });
      if ($scope.lightData.length > 0) {
        $scope.currentLight = $scope.lightData[$scope.lightData.length - 1][1];
      }
    }
    if (typeof data.tempReadings != 'undefined') {
      data.tempReadings.forEach(function(reading) {
        $scope.tempData.push([reading.createdAt, reading.reading]);
      });
      if ($scope.tempData.length > 0) {
        $scope.currentTemp = $scope.tempData[$scope.tempData.length - 1][1];
      }
    }
    if (typeof data.update != 'undefined') {
      if (typeof data.update.light != 'undefined') {
        $scope.currentLight = data.update.light;
        $scope.lightData.push([parseInt(moment().format('x')), data.update.light]);
        if ($scope.lightData.length > 300) {
          $scope.lightData.shift();
        }
      }
      if (typeof data.update.temp != 'undefined') {
        $scope.currentTemp = data.update.temp;
        $scope.tempData.push([parseInt(moment().format('x')), data.update.temp]);
        if ($scope.tempData.length > 300) {
          $scope.tempData.shift();
        }
      }
      if (typeof data.update.status != 'undefined') {
        for (var i = 0; i < $scope.items.length; i++) {
          if ($scope.items[i].name === data.update.name) {
            $scope.items[i].status = data.update.status;
            $scope.items[i].updated = parseInt(moment().format('X'));
          }
        }
      }
    }
    $scope.safeApply();
  });

  $scope.tempChart = {
    options: {
      chart: { type: 'spline' },
      tooltip: {
        hideDelay: 50,
        dateTimeLabelFormats: {
          millisecond: '%A, %b %e, %l:%M:%S %P',
          second: '%A, %b %e, %l:%M:%S %P',
          minute: '%A, %b %e, %l:%M %P',
          hour: '%A, %b %e, %l:%M %P',
          day: '%A, %b %e, %Y',
          week: 'Week from %A, %b %e, %Y',
          month: '%b \'%y',
          year: '%Y'
        }
      }
    },
    plotOptions: { series: { marker: { enabled: false } } },
    xAxis: {
      type: 'datetime',
      dateTimeLabelFormats: {
        millisecond: '%l:%M:%S.%L %P',
        second: '%l:%M:%S %P',
        minute: '%l:%M %P',
        hour: '%l:%M %P',
        day: '%e. %b',
        week: '%e. %b',
        month: '%b \'%y',
        year: '%Y'
      }
    },
    yAxis: {
      startOnTick: false,
      showFirstLabel: false,
      showLastLabel: false,
      minPadding: 0.2,
      maxPadding: 0.2,
      title: {
        text: 'Degrees Farenheit'
      },
      plotBands: [
        { // freezing & below
          color: '#eef',
          from: -50,
          to: 32
        },
        { // cold
          color: '#dde',
          from: 40,
          to: 32.1
        },
        { // mark warm
          color: '#ffe',
          from: 75,
          to: 89.9
        },
        { // mark hot
          color: '#fee',
          from: 90,
          to: 150
        }
      ]
    },
    title: {
      text: "Recent Temperature"
    },
    series: [{
      name: 'Temperature Reading',
      color: '#d55',
      data: $scope.tempData
    }]
  };

$scope.lightChart = {
    options: {
      chart: { type: 'spline' },
      tooltip: {
        hideDelay: 50,
        dateTimeLabelFormats: {
          millisecond: '%A, %b %e, %l:%M:%S %P',
          second: '%A, %b %e, %l:%M:%S %P',
          minute: '%A, %b %e, %l:%M %P',
          hour: '%A, %b %e, %l:%M %P',
          day: '%A, %b %e, %Y',
          week: 'Week from %A, %b %e, %Y',
          month: '%b \'%y',
          year: '%Y'
        }
      }
    },
    plotOptions: { series: { marker: { enabled: false } } },
    xAxis: {
      type: 'datetime',
      dateTimeLabelFormats: {
        millisecond: '%l:%M:%S.%L %P',
        second: '%l:%M:%S %P',
        minute: '%l:%M %P',
        hour: '%l:%M %P',
        day: '%e. %b',
        week: '%e. %b',
        month: '%b \'%y',
        year: '%Y'
      }
    },
    yAxis: {
      floor: 0,
      ceiling: 100,
      minPadding: 0.2,
      maxPadding: 0.2,
      title: { text: 'Percentage' }
    },
    title: { text: "Recent Light Level" },
    series: [{
      name: 'Brightness Reading',
      data: $scope.lightData
    }]
  };

  $scope.itemType = function(status) {
    if (status == 'on' || status == 'off') {
      return 1;
    }
    else {
      return 2;
    }
  };


  $scope.$watch(function(){
     $scope.windowWidth = $window.innerWidth;
  });

}]);

app.filter('capitalize', function() {
  return function(input, all) {
    return (!!input) ? input.replace(/([^\W_]+[^\s-]*) */g, function(txt){return txt.charAt(0).toUpperCase() + txt.substr(1).toLowerCase();}) : '';
  }
});
