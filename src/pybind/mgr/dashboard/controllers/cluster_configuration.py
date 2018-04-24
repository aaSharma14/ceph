# -*- coding: utf-8 -*-
from __future__ import absolute_import

import cherrypy

from . import ApiController, RESTController
from .. import mgr


@ApiController('/cluster_conf')
class ClusterConfiguration(RESTController):
    def list(self):
        options = mgr.get("config_options")['options']
        return options

    def get(self, name):
        for option in mgr.get('config_options')['options']:
            if option['name'] == name:
                return option

        raise cherrypy.HTTPError(404)
